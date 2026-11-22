/**
 Concurrent Order Processor library - Google Test

 Authors: dudleylane, Claude

 Copyright (C) 2026 dudleylane

 Distributed under the GNU Affero General Public License (AGPL).
*/

// The server's JSON messages against the frontend's TypeScript types for them (#638). The frontend gives each parsed
// message its hand-written type unchecked, so a field the server renamed, added or dropped reached the UI unnoticed.
// Each message here is written by the server's own serializer, an execution report once per C++ class (#179), and is
// compared both ways with the type src/oms-frontend/src/types declares for it: every field the server writes must be
// declared there, with a type its value fits, and every field declared required (not ?:) must be written. The types
// are read at run time by a parser that knows only the constructs those files use and throws on anything else, naming
// it, so that a new construct is taught to it rather than misread. What the client sends is out of scope.
//
// A message may leave out a field declared optional (?:), but one that every message leaves out is compared with
// nothing, and the server could drop it, or write it as another type, unnoticed. So each must be in one of the
// messages. On FX they are a swap order's settlement dates and far price, and a swap leg's execLegType, which the
// serializer writes for a swap alone: the messages include a swap, and a report of each of its legs.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DataModelDef.h"
#include "EnumStrings.h"
#include "JsonSerializer.h"
#include "OrderStorage.h"
#include "TestAux.h"
#include "TestFixtures.h"
#include "WideDataStorage.h"

using namespace COP;
using namespace COP::Store;
using json = nlohmann::json;

namespace
{

/// What a refusal says after naming the construct
const char *const TEACH_IT = " (WireContractTest's parser knows only the TypeScript that src/oms-frontend/src/types "
                             "uses, #638: teach it this construct)";

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        throw std::runtime_error("can't read " + path.string());
    }
    std::stringstream text;
    text << f.rdbuf();
    return text.str();
}

// ---------------------------------------------------------------------------------------------------------------------
// The frontend's type files: their tokens, then their declarations

struct Token
{
    enum Kind
    {
        END,
        WORD,   ///< an identifier or a keyword
        STRING, ///< a string literal, without its quotes
        NUMBER,
        PUNCT ///< one character of punctuation
    };
    Kind kind;
    std::string text;
    int line;
    int column;
};

/// A file's tokens, past its whitespace and comments. Refused here: a template literal, an escape in a string, and a
/// byte outside ASCII anywhere but in a comment or a string. The parser names whatever else it doesn't know.
std::vector<Token> tokenize(const std::string &file, const std::string &text)
{
    std::vector<Token> tokens;
    int line = 1;
    size_t lineStart = 0;
    size_t i = 0;
    const auto refusal = [&](const std::string &construct)
    {
        return std::runtime_error(file + ":" + std::to_string(line) + ":" + std::to_string(i - lineStart + 1) +
                                  ": unsupported construct: " + construct + TEACH_IT);
    };
    const auto isWordChar = [](char c)
    {
        return (0 != std::isalnum(static_cast<unsigned char>(c))) || ('_' == c) || ('$' == c);
    };
    while (i < text.size())
    {
        const char c = text[i];
        const char next = (i + 1 < text.size()) ? text[i + 1] : '\0';
        if ('\n' == c)
        {
            ++line;
            lineStart = ++i;
            continue;
        }
        if ((' ' == c) || ('\t' == c) || ('\r' == c))
        {
            ++i;
            continue;
        }
        if (('/' == c) && ('/' == next))
        {
            i = std::min(text.find('\n', i), text.size());
            continue;
        }
        if (('/' == c) && ('*' == next))
        {
            const size_t end = text.find("*/", i + 2);
            if (std::string::npos == end)
            {
                throw refusal("a comment that doesn't end");
            }
            for (; i < end + 2; ++i)
            {
                if ('\n' == text[i])
                {
                    ++line;
                    lineStart = i + 1;
                }
            }
            continue;
        }
        Token token{ Token::PUNCT, std::string(1, c), line, static_cast<int>(i - lineStart + 1) };
        if (isWordChar(c) && (0 == std::isdigit(static_cast<unsigned char>(c))))
        {
            const size_t start = i;
            while ((i < text.size()) && isWordChar(text[i]))
            {
                ++i;
            }
            token.kind = Token::WORD;
            token.text = text.substr(start, i - start);
        }
        else if (0 != std::isdigit(static_cast<unsigned char>(c)))
        {
            const size_t start = i;
            while ((i < text.size()) && (isWordChar(text[i]) || ('.' == text[i])))
            {
                ++i;
            }
            token.kind = Token::NUMBER;
            token.text = text.substr(start, i - start);
        }
        else if (('\'' == c) || ('"' == c))
        {
            const size_t end = text.find_first_of(std::string(1, c) + "\\\n", i + 1);
            if ((std::string::npos == end) || ('\n' == text[end]))
            {
                throw refusal("a string literal that doesn't end on its line");
            }
            if ('\\' == text[end])
            {
                throw refusal("an escape in a string literal");
            }
            token.kind = Token::STRING;
            token.text = text.substr(i + 1, end - i - 1);
            i = end + 1;
        }
        else if ('`' == c)
        {
            throw refusal("a template literal");
        }
        else if ((c < '!') || (c > '~'))
        {
            throw refusal("the byte " + std::to_string(static_cast<unsigned char>(c)) +
                          " outside a comment or a string");
        }
        else
        {
            ++i;
        }
        tokens.push_back(token);
    }
    tokens.push_back(Token{ Token::END, "", line, static_cast<int>(i - lineStart + 1) });
    return tokens;
}

struct TsType;
using TsTypeP = std::shared_ptr<const TsType>;

struct TsField
{
    std::string name;
    bool optional = false;
    TsTypeP type;
    std::string where; ///< file:line
};

/// An interface, or an object type written in place, as each ServerMessage variant is
struct TsObject
{
    std::string name;    ///< an interface's; empty for an object type written in place
    std::string extends; ///< the interface an interface extends, if it extends one
    std::vector<TsField> fields;
    std::string where;
};

struct TsType
{
    enum Kind
    {
        PRIMITIVE,   ///< number, string or boolean, in name
        LITERAL,     ///< a string literal type, its text in name
        REF,         ///< an interface, an enum or a type alias, by name
        ENUM_MEMBER, ///< name.member, as ExecType.TRADE
        ARRAY,       ///< of[0][]
        OBJECT,      ///< object, written in place
        UNION        ///< of[0] | of[1] | ...
    };
    Kind kind = PRIMITIVE;
    std::string name;
    std::string member;
    std::vector<TsTypeP> of;
    std::shared_ptr<const TsObject> object;
    std::string where;
};

/// What the frontend's type files declare, in one namespace for the three, as their imports make it
struct TsDeclarations
{
    std::map<std::string, std::shared_ptr<const TsObject>> interfaces;
    std::map<std::string, TsTypeP> aliases;
    std::map<std::string, std::map<std::string, std::string>> enums; ///< enum -> member -> value
    std::map<std::string, std::string> declaredAt;                   ///< every name declared, functions' too
};

std::string describe(const TsType &type)
{
    switch (type.kind)
    {
    case TsType::PRIMITIVE:
    case TsType::REF:
        return type.name;
    case TsType::LITERAL:
        return "'" + type.name + "'";
    case TsType::ENUM_MEMBER:
        return type.name + "." + type.member;
    case TsType::ARRAY:
        return describe(*type.of[0]) + "[]";
    case TsType::OBJECT:
        return "{ ... }";
    case TsType::UNION:
        break;
    }
    std::string text;
    for (const TsTypeP &alternative : type.of)
    {
        text += (text.empty() ? "" : " | ") + describe(*alternative);
    }
    return text;
}

/// One of the frontend's type files, its declarations added to those of the others. It reads exactly the constructs
/// those files use:
///   import [type] { A, B, } from '...';             export * from '...';
///   export interface A [extends B] { f: T; g?: T; }  export type A = T;
///   export enum A { M = 'm', }                       export function f(p: T): p is T { ... }
/// where a type T is number, string, boolean, a string literal, a name, Enum.Member, import('...').Name, T[], an object
/// type written in place ({ f: T; g: T }), or a union of those, a leading | allowed. A function's body isn't read, its
/// braces only matched: it declares no type. Anything else throws, naming the construct, where it is, and the
/// declaration and field it is in.
class TsParser
{
public:
    TsParser(const std::string &file, const std::string &text, TsDeclarations &into)
        : file_(file), tokens_(tokenize(file, text)), into_(into)
    {
    }

    void parse()
    {
        while (Token::END != peek().kind)
        {
            if (isWord("import"))
            {
                parseImport();
            }
            else if (isWord("export"))
            {
                parseExport();
            }
            else
            {
                unsupported("a top-level " + quote(peek()) + ", where only imports and exports are read");
            }
        }
    }

private:
    /// Names what is being read while it lives, for a refusal to say where it is
    class Within
    {
    public:
        Within(std::vector<std::string> &context, const std::string &what) : context_(context)
        {
            context_.push_back(what);
        }
        ~Within()
        {
            context_.pop_back();
        }
        Within(const Within &) = delete;
        Within &operator=(const Within &) = delete;

    private:
        std::vector<std::string> &context_;
    };

    const Token &peek(size_t ahead = 0) const
    {
        return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)];
    }
    bool isWord(const char *word, size_t ahead = 0) const
    {
        return (Token::WORD == peek(ahead).kind) && (word == peek(ahead).text);
    }
    bool isPunct(char c) const
    {
        return (Token::PUNCT == peek().kind) && (c == peek().text[0]);
    }
    Token take()
    {
        const Token token = peek();
        pos_ = std::min(pos_ + 1, tokens_.size() - 1);
        return token;
    }
    static std::string quote(const Token &token)
    {
        if (Token::END == token.kind)
        {
            return "the end of the file";
        }
        return ((Token::STRING == token.kind) ? "the string '" : "'") + token.text + "'";
    }
    std::string where(const Token &token) const
    {
        return file_ + ":" + std::to_string(token.line);
    }
    [[noreturn]] void unsupported(const std::string &construct) const
    {
        unsupported(construct, peek());
    }
    [[noreturn]] void unsupported(const std::string &construct, const Token &at) const
    {
        std::string text = file_ + ":" + std::to_string(at.line) + ":" + std::to_string(at.column) + ": ";
        for (size_t k = 0; k < context_.size(); ++k)
        {
            text += ((0 == k) ? "" : ", ") + context_[k];
        }
        throw std::runtime_error(text + (context_.empty() ? "" : ": ") + "unsupported construct: " + construct +
                                 TEACH_IT);
    }
    void expectPunct(char c)
    {
        if (!isPunct(c))
        {
            unsupported(quote(peek()) + " where '" + std::string(1, c) + "' belongs");
        }
        take();
    }
    void expectWord(const char *word)
    {
        if (!isWord(word))
        {
            unsupported(quote(peek()) + " where '" + word + "' belongs");
        }
        take();
    }
    std::string expectName(const char *what)
    {
        if (Token::WORD != peek().kind)
        {
            unsupported(quote(peek()) + " where " + what + " belongs");
        }
        return take().text;
    }
    void expectString(const char *what)
    {
        if (Token::STRING != peek().kind)
        {
            unsupported(quote(peek()) + " where " + what + " belongs");
        }
        take();
    }
    /// A name declared twice would be TypeScript's declaration merging, which the check doesn't follow
    void declare(const std::string &name, const Token &at)
    {
        const auto [declared, added] = into_.declaredAt.emplace(name, where(at));
        if (!added)
        {
            unsupported("a second declaration of " + name + ", declared at " + declared->second, at);
        }
    }

    void parseImport()
    {
        take(); // import
        if (isWord("type"))
        {
            take();
        }
        if (!isPunct('{'))
        {
            unsupported("an import of " + quote(peek()) + ", where only names in braces are read");
        }
        take();
        while (!isPunct('}'))
        {
            expectName("an imported name");
            if (isPunct(','))
            {
                take();
            }
            else if (!isPunct('}'))
            {
                unsupported(quote(peek()) + " in an import's names");
            }
        }
        take();
        expectWord("from");
        expectString("the module");
        expectPunct(';');
    }

    void parseExport()
    {
        take(); // export
        if (isPunct('*'))
        {
            take();
            expectWord("from");
            expectString("the module");
            expectPunct(';');
        }
        else if (isWord("interface"))
        {
            parseInterface();
        }
        else if (isWord("type"))
        {
            parseTypeAlias();
        }
        else if (isWord("enum"))
        {
            parseEnum();
        }
        else if (isWord("function"))
        {
            parseFunction();
        }
        else
        {
            unsupported("an export of " + quote(peek()) +
                        ", where only interfaces, type aliases, enums, functions and export * are read");
        }
    }

    void parseInterface()
    {
        take(); // interface
        const Token at = peek();
        auto object = std::make_shared<TsObject>();
        object->name = expectName("the interface's name");
        object->where = where(at);
        Within within(context_, "interface " + object->name);
        if (isPunct('<'))
        {
            unsupported("a generic interface");
        }
        if (isWord("extends"))
        {
            take();
            object->extends = expectName("the interface it extends");
            if (isPunct('<'))
            {
                unsupported("generic type arguments on " + object->extends);
            }
            if (isPunct(','))
            {
                unsupported("an interface that extends more than one");
            }
        }
        expectPunct('{');
        object->fields = parseFields();
        declare(object->name, at);
        into_.interfaces[object->name] = object;
    }

    /// An object type's fields, from past its { to past its }: each name[?]: T, ended by ; but for the last's
    std::vector<TsField> parseFields()
    {
        std::vector<TsField> fields;
        while (!isPunct('}'))
        {
            if (isPunct('['))
            {
                unsupported("an index signature");
            }
            if (Token::WORD != peek().kind)
            {
                unsupported(quote(peek()) + " where a field's name belongs");
            }
            const Token name = take();
            TsField field;
            field.name = name.text;
            field.where = where(name);
            Within within(context_, "field " + field.name);
            if (Token::WORD == peek().kind)
            {
                context_.back() = "field " + peek().text;
                unsupported("the modifier '" + name.text + "'", name);
            }
            if (isPunct('?'))
            {
                take();
                field.optional = true;
            }
            if (isPunct('(') || isPunct('<'))
            {
                unsupported("a method signature");
            }
            expectPunct(':');
            field.type = parseType();
            for (const TsField &other : fields)
            {
                if (other.name == field.name)
                {
                    unsupported("a second field " + field.name + ", declared at " + other.where, name);
                }
            }
            fields.push_back(field);
            if (isPunct(';'))
            {
                take();
            }
            else if (isPunct(','))
            {
                unsupported("',' between fields, where these files use ';'");
            }
            else if (!isPunct('}'))
            {
                unsupported(quote(peek()) + " after a field's type");
            }
        }
        take();
        return fields;
    }

    void parseTypeAlias()
    {
        take(); // type
        const Token at = peek();
        const std::string name = expectName("the type's name");
        Within within(context_, "type " + name);
        if (isPunct('<'))
        {
            unsupported("a generic type alias");
        }
        expectPunct('=');
        TsTypeP type = parseType();
        expectPunct(';');
        declare(name, at);
        into_.aliases[name] = type;
    }

    void parseEnum()
    {
        take(); // enum
        const Token at = peek();
        const std::string name = expectName("the enum's name");
        Within within(context_, "enum " + name);
        expectPunct('{');
        std::map<std::string, std::string> members;
        while (!isPunct('}'))
        {
            const Token member = peek();
            Within withinMember(context_, "member " + expectName("an enum member"));
            if (!isPunct('='))
            {
                unsupported("an enum member without a value");
            }
            take();
            if (Token::STRING != peek().kind)
            {
                unsupported("an enum member whose value isn't a string literal: " + quote(peek()));
            }
            if (!members.emplace(member.text, take().text).second)
            {
                unsupported("a second member " + member.text, member);
            }
            if (isPunct(','))
            {
                take();
            }
            else if (!isPunct('}'))
            {
                unsupported(quote(peek()) + " after an enum member");
            }
        }
        take();
        declare(name, at);
        into_.enums[name] = members;
    }

    /// A type guard, as index.ts's is* functions are: (exec: Execution): exec is RejectExecution
    void parseFunction()
    {
        take(); // function
        const Token at = peek();
        const std::string name = expectName("the function's name");
        Within within(context_, "function " + name);
        if (isPunct('<'))
        {
            unsupported("a generic function");
        }
        expectPunct('(');
        const std::string parameter = expectName("its parameter");
        if (isPunct('?') || isPunct('='))
        {
            unsupported("an optional parameter");
        }
        expectPunct(':');
        parseType();
        if (isPunct(','))
        {
            unsupported("a function of more than one parameter");
        }
        expectPunct(')');
        expectPunct(':');
        if (!isWord(parameter.c_str()) || !isWord("is", 1))
        {
            unsupported("a return type that isn't a type predicate on " + parameter);
        }
        take();
        take();
        parseType();
        expectPunct('{');
        for (int depth = 1; 0 < depth;)
        {
            if (Token::END == peek().kind)
            {
                unsupported("a function body that doesn't end");
            }
            const Token token = take();
            if (Token::PUNCT == token.kind)
            {
                depth += ("{" == token.text) ? 1 : (("}" == token.text) ? -1 : 0);
            }
        }
        declare(name, at);
    }

    TsTypeP parseType()
    {
        const Token at = peek();
        if (isPunct('|'))
        {
            take(); // a leading |, as a union laid out an alternative a line has
        }
        std::vector<TsTypeP> alternatives{ parseArrayType() };
        while (isPunct('|'))
        {
            take();
            alternatives.push_back(parseArrayType());
        }
        if (isPunct('&'))
        {
            unsupported("an intersection type");
        }
        if (1 == alternatives.size())
        {
            return alternatives[0];
        }
        auto type = std::make_shared<TsType>();
        type->kind = TsType::UNION;
        type->of = alternatives;
        type->where = where(at);
        return type;
    }

    TsTypeP parseArrayType()
    {
        TsTypeP type = parsePrimaryType();
        while (isPunct('['))
        {
            const Token at = take();
            if (!isPunct(']'))
            {
                unsupported("an indexed access type");
            }
            take();
            auto array = std::make_shared<TsType>();
            array->kind = TsType::ARRAY;
            array->of = { type };
            array->where = where(at);
            type = array;
        }
        return type;
    }

    TsTypeP parsePrimaryType()
    {
        const Token at = peek();
        auto type = std::make_shared<TsType>();
        type->where = where(at);
        if (Token::STRING == at.kind)
        {
            type->kind = TsType::LITERAL;
            type->name = take().text;
            return type;
        }
        if (isPunct('{'))
        {
            take();
            auto object = std::make_shared<TsObject>();
            object->where = where(at);
            object->fields = parseFields();
            type->kind = TsType::OBJECT;
            type->object = object;
            return type;
        }
        if (Token::NUMBER == at.kind)
        {
            unsupported("a numeric literal type");
        }
        if (isPunct('('))
        {
            unsupported("a parenthesised or function type");
        }
        if (isPunct('['))
        {
            unsupported("a tuple type");
        }
        if (Token::WORD != at.kind)
        {
            unsupported(quote(at) + " where a type belongs");
        }
        static const std::set<std::string> others{ "any",    "unknown",  "null",   "undefined", "never", "object",
                                                   "void",   "bigint",   "symbol", "true",      "false", "keyof",
                                                   "typeof", "readonly", "unique", "infer",     "this",  "asserts" };
        if (0 < others.count(at.text))
        {
            unsupported("the type '" + at.text + "'");
        }
        take();
        if (("number" == at.text) || ("string" == at.text) || ("boolean" == at.text))
        {
            type->name = at.text;
            return type;
        }
        type->kind = TsType::REF;
        type->name = at.text;
        if ("import" == at.text)
        {
            // import('./index').NewOrderRequest, as websocket.ts names the requests
            expectPunct('(');
            expectString("the module");
            expectPunct(')');
            expectPunct('.');
            type->name = expectName("the imported type's name");
        }
        if (isPunct('.'))
        {
            take();
            type->kind = TsType::ENUM_MEMBER;
            type->member = expectName("the enum's member");
            if (isPunct('.'))
            {
                unsupported("a name qualified more than once");
            }
        }
        if (isPunct('<'))
        {
            unsupported("generic type arguments on " + type->name);
        }
        return type;
    }

    std::string file_;
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    std::vector<std::string> context_;
    TsDeclarations &into_;
};

void validateType(const TsDeclarations &ts, const TsType &type, const std::string &context)
{
    const auto refusal = [&](const char *why)
    {
        return std::runtime_error(type.where + ": " + context + ": " + describe(type) + why);
    };
    switch (type.kind)
    {
    case TsType::REF:
        if ((0 == ts.interfaces.count(type.name)) && (0 == ts.aliases.count(type.name)) &&
            (0 == ts.enums.count(type.name)))
        {
            throw refusal(" names nothing the frontend's type files declare");
        }
        break;
    case TsType::ENUM_MEMBER:
        if ((0 == ts.enums.count(type.name)) || (0 == ts.enums.at(type.name).count(type.member)))
        {
            throw refusal(" names no member of an enum the frontend's type files declare");
        }
        break;
    case TsType::ARRAY:
    case TsType::UNION:
        for (const TsTypeP &of : type.of)
        {
            validateType(ts, *of, context);
        }
        break;
    case TsType::OBJECT:
        for (const TsField &field : type.object->fields)
        {
            validateType(ts, *field.type, context + ", field " + field.name);
        }
        break;
    case TsType::PRIMITIVE:
    case TsType::LITERAL:
        break;
    }
}

/// Every name the declarations use names one of them, and no interface extends itself
void validate(const TsDeclarations &ts)
{
    for (const auto &[name, object] : ts.interfaces)
    {
        std::set<std::string> seen{ name };
        for (const TsObject *derived = object.get(); !derived->extends.empty();)
        {
            const auto base = ts.interfaces.find(derived->extends);
            if (ts.interfaces.end() == base)
            {
                throw std::runtime_error(derived->where + ": interface " + derived->name + " extends " +
                                         derived->extends + ", which the frontend's type files declare no interface");
            }
            if (!seen.insert(derived->extends).second)
            {
                throw std::runtime_error(object->where + ": interface " + name + " extends itself");
            }
            derived = base->second.get();
        }
        for (const TsField &field : object->fields)
        {
            validateType(ts, *field.type, "interface " + name + ", field " + field.name);
        }
    }
    for (const auto &[name, type] : ts.aliases)
    {
        validateType(ts, *type, "type " + name);
    }
}

/// The frontend's declarations, from the three files that declare what the wire carries
TsDeclarations readFrontend()
{
    TsDeclarations ts;
    const std::filesystem::path types = test::sourceRoot() / "src/oms-frontend/src/types";
    for (const char *file : { "enums.ts", "index.ts", "websocket.ts" })
    {
        TsParser(file, readFile(types / file), ts).parse();
    }
    validate(ts);
    return ts;
}

// ---------------------------------------------------------------------------------------------------------------------
// The check: a message against its type, both ways

enum class Problem
{
    UNDECLARED, ///< the server writes what the frontend doesn't declare
    MISSING,    ///< the frontend requires what the server doesn't write
    MISTYPED,   ///< the server writes a value the declared type doesn't take
    UNCHECKED   ///< a list with no element, or an optional field no message has, which nothing compares
};

struct Finding
{
    Problem problem;
    std::string text;
};

/// A message as the server's serializer wrote it
struct Message
{
    std::string name; ///< as findings name it: its type, and an execution report's class and type
    std::string text;
    std::string reportClass;   ///< an execution report's C++ class
    std::string dataInterface; ///< what its data is compared with, where that isn't what ServerMessage declares
};

/// The frontend's interface for each class of execution report the serializer writes. A report carries its class's
/// fields, not its type's, so a refused replace (type REPLACE) is a RejectExecution and an order cancel (type CANCEL)
/// a plain ExecutionReport; the frontend tells them apart with index.ts's is* guards (#179).
const std::map<std::string, std::string> REPORT_INTERFACES{
    { "ExecutionEntry", "ExecutionReport" },        { "TradeExecEntry", "TradeExecution" },
    { "RejectExecEntry", "RejectExecution" },       { "ReplaceExecEntry", "ReplaceExecution" },
    { "ExecCorrectExecEntry", "CorrectExecution" }, { "TradeCancelExecEntry", "TradeCancelExecution" },
};

/// ServerMessage's variants by the type each names: websocket.ts's union of what the server sends
std::map<std::string, const TsObject *> serverVariants(const TsDeclarations &ts)
{
    const auto alias = ts.aliases.find("ServerMessage");
    if (ts.aliases.end() == alias)
    {
        throw std::runtime_error("websocket.ts declares no ServerMessage");
    }
    const TsType &type = *alias->second;
    std::map<std::string, const TsObject *> variants;
    for (const TsTypeP &alternative : (TsType::UNION == type.kind) ? type.of : std::vector<TsTypeP>{ alias->second })
    {
        const TsField *tag = nullptr;
        if (TsType::OBJECT == alternative->kind)
        {
            for (const TsField &field : alternative->object->fields)
            {
                tag = (("type" == field.name) && (TsType::LITERAL == field.type->kind)) ? &field : tag;
            }
        }
        if (nullptr == tag)
        {
            throw std::runtime_error(alternative->where + ": ServerMessage's " + describe(*alternative) +
                                     " isn't an object type with a string literal type, by which a message's variant "
                                     "is found");
        }
        if (!variants.emplace(tag->type->name, alternative->object.get()).second)
        {
            throw std::runtime_error(alternative->where + ": ServerMessage's second '" + tag->type->name + "' variant");
        }
    }
    return variants;
}

/// An object type's fields: an interface's own after those it inherits, each replacing one of the same name, as
/// TradeExecution's type narrows ExecutionReport's
std::vector<const TsField *> fieldsOf(const TsDeclarations &ts, const TsObject &object)
{
    std::vector<const TsField *> fields;
    if (!object.extends.empty())
    {
        fields = fieldsOf(ts, *ts.interfaces.at(object.extends));
    }
    for (const TsField &field : object.fields)
    {
        const auto same = std::find_if(fields.begin(), fields.end(),
                                       [&field](const TsField *inherited)
                                       {
                                           return inherited->name == field.name;
                                       });
        if (fields.end() == same)
        {
            fields.push_back(&field);
        }
        else
        {
            *same = &field;
        }
    }
    return fields;
}

const TsField *findField(const std::vector<const TsField *> &fields, const std::string &name)
{
    const auto found = std::find_if(fields.begin(), fields.end(),
                                    [&name](const TsField *field)
                                    {
                                        return field->name == name;
                                    });
    return (fields.end() == found) ? nullptr : *found;
}

bool isOrExtends(const TsDeclarations &ts, const std::string &interface, const std::string &base)
{
    for (auto it = ts.interfaces.find(interface); ts.interfaces.end() != it;
         it = ts.interfaces.find(it->second->extends))
    {
        if (base == it->first)
        {
            return true;
        }
    }
    return false;
}

std::string kindOf(const json &value)
{
    if (value.is_string())
    {
        return "a string";
    }
    if (value.is_number())
    {
        return "a number";
    }
    if (value.is_boolean())
    {
        return "a boolean";
    }
    if (value.is_object())
    {
        return "an object";
    }
    return value.is_array() ? "an array" : "null";
}

/// What the messages were compared with, across them all, for the fields none of them has
struct Coverage
{
    /// Each object type a value was compared with, as findings name it
    std::map<const TsObject *, std::string> compared;
    /// Each of their fields a value had, where it is declared
    std::set<const TsField *> written;
};

/// Compares a message with the type the frontend declares for it. Each finding names the message, the path to the
/// field in it, the interface, and the field.
class Checker
{
public:
    Checker(const TsDeclarations &ts, const std::string &message, std::vector<Finding> &findings,
            Coverage *coverage = nullptr)
        : ts_(ts), message_(message), findings_(findings), coverage_(coverage)
    {
    }

    /// The message against ServerMessage's variant for its type, its data against dataInterface if one is given
    void checkMessage(const json &msg, const std::string &dataInterface)
    {
        if (!msg.is_object() || !msg.contains("type") || !msg["type"].is_string())
        {
            add(Problem::UNDECLARED, "the server writes a message with no type, by which ServerMessage's variants are "
                                     "told apart");
            return;
        }
        const std::string type = msg["type"];
        const auto variants = serverVariants(ts_);
        const auto variant = variants.find(type);
        if (variants.end() == variant)
        {
            add(Problem::UNDECLARED, "the server sends a message of type '" + type + "', which ServerMessage (" +
                                         ts_.aliases.at("ServerMessage")->where + ") has no variant for");
            return;
        }
        const std::string display = "ServerMessage's '" + type + "' variant (" + variant->second->where + ")";
        if (dataInterface.empty())
        {
            checkObject(msg, *variant->second, display, "", nullptr);
            return;
        }
        // An execution report's data is compared with its class's interface (#179), which must be what ServerMessage
        // declares for data or an interface extending it
        if (0 == ts_.interfaces.count(dataInterface))
        {
            add(Problem::UNDECLARED, "the frontend declares no interface " + dataInterface + ", which the report's " +
                                         "class is read as (REPORT_INTERFACES)");
            return;
        }
        const TsField *data = findField(fieldsOf(ts_, *variant->second), "data");
        if ((nullptr == data) || (TsType::REF != data->type->kind) ||
            !isOrExtends(ts_, dataInterface, data->type->name))
        {
            add(Problem::MISTYPED, display +
                                       " declares data: " + ((nullptr == data) ? "nothing" : describe(*data->type)) +
                                       ", which " + dataInterface + " neither is nor extends");
        }
        checkObject(msg, *variant->second, display, "", &dataInterface);
    }

    void checkValue(const json &value, const TsType &type, const std::string &path, const std::string &owner,
                    const TsField &field)
    {
        const auto mistyped = [&](const std::string &as)
        {
            add(Problem::MISTYPED, "the server writes " + path + " as " + as + ", where " + owner + " declares " +
                                       field.name + ": " + describe(type) + " (" + field.where + ")");
        };
        switch (type.kind)
        {
        case TsType::PRIMITIVE:
            if ((("number" == type.name) && !value.is_number()) || (("string" == type.name) && !value.is_string()) ||
                (("boolean" == type.name) && !value.is_boolean()))
            {
                mistyped(kindOf(value));
            }
            return;
        case TsType::LITERAL:
            if (!value.is_string() || (type.name != value.get<std::string>()))
            {
                mistyped(value.dump());
            }
            return;
        case TsType::ENUM_MEMBER:
            if (!value.is_string() || (ts_.enums.at(type.name).at(type.member) != value.get<std::string>()))
            {
                mistyped(value.dump());
            }
            return;
        case TsType::REF:
            if (0 < ts_.interfaces.count(type.name))
            {
                const TsObject &object = *ts_.interfaces.at(type.name);
                checkObject(value, object, type.name + " (" + object.where + ")", path, nullptr);
            }
            else if (0 < ts_.enums.count(type.name))
            {
                // A string: whether the enum has the value is EnumCouplingTest's to say
                if (!value.is_string())
                {
                    mistyped(kindOf(value));
                }
            }
            else
            {
                checkValue(value, *ts_.aliases.at(type.name), path, owner, field);
            }
            return;
        case TsType::ARRAY:
            if (!value.is_array())
            {
                mistyped(kindOf(value));
            }
            else if (value.empty())
            {
                add(Problem::UNCHECKED, path + " is empty, so nothing compares its elements with " +
                                            describe(*type.of[0]) + ": give it one in the test's message");
            }
            for (size_t i = 0; value.is_array() && (i < value.size()); ++i)
            {
                checkValue(value[i], *type.of[0], path + "[" + std::to_string(i) + "]", owner, field);
            }
            return;
        case TsType::OBJECT:
            checkObject(value, *type.object, owner + "'s " + field.name + " (" + type.object->where + ")", path,
                        nullptr);
            return;
        case TsType::UNION:
            for (const TsTypeP &alternative : type.of)
            {
                std::vector<Finding> trial;
                Coverage fitting; // counted only for the alternative the value fits
                Checker(ts_, message_, trial, &fitting).checkValue(value, *alternative, path, owner, field);
                if (trial.empty())
                {
                    if (nullptr != coverage_)
                    {
                        coverage_->compared.insert(fitting.compared.begin(), fitting.compared.end());
                        coverage_->written.insert(fitting.written.begin(), fitting.written.end());
                    }
                    return;
                }
            }
            mistyped(value.is_primitive() ? value.dump() : kindOf(value));
            return;
        }
    }

    /// Both ways: each field the value has is declared, and each field declared required is there
    void checkObject(const json &value, const TsObject &object, const std::string &display, const std::string &path,
                     const std::string *dataInterface)
    {
        const std::string in = path.empty() ? "the message" : path;
        if (!value.is_object())
        {
            add(Problem::MISTYPED,
                "the server writes " + in + " as " + kindOf(value) + ", where " + display + " is an object type");
            return;
        }
        const std::vector<const TsField *> fields = fieldsOf(ts_, object);
        if (nullptr != coverage_)
        {
            coverage_->compared.emplace(&object, display);
        }
        for (auto it = value.begin(); value.end() != it; ++it)
        {
            const std::string itemPath = path.empty() ? it.key() : path + "." + it.key();
            const TsField *field = findField(fields, it.key());
            if ((nullptr != field) && (nullptr != coverage_))
            {
                coverage_->written.insert(field);
            }
            if (nullptr == field)
            {
                add(Problem::UNDECLARED, "the server writes " + itemPath + ", which " + display + " doesn't declare");
            }
            else if ((nullptr != dataInterface) && ("data" == it.key()))
            {
                const TsObject &report = *ts_.interfaces.at(*dataInterface);
                checkObject(it.value(), report, *dataInterface + " (" + report.where + ")", itemPath, nullptr);
            }
            else
            {
                checkValue(it.value(), *field->type, itemPath, display, *field);
            }
        }
        for (const TsField *field : fields)
        {
            if (!field->optional && !value.contains(field->name))
            {
                add(Problem::MISSING, display + " requires " + field->name + " (" + field->where +
                                          "), which the server doesn't write in " + in);
            }
        }
    }

private:
    void add(Problem problem, const std::string &text)
    {
        findings_.push_back(Finding{ problem, message_ + ": " + text });
    }

    const TsDeclarations &ts_;
    const std::string message_;
    std::vector<Finding> &findings_;
    Coverage *coverage_; ///< what the check compared, if it is kept
};

std::vector<Finding> check(const TsDeclarations &ts, const std::vector<Message> &messages, Coverage *coverage = nullptr)
{
    std::vector<Finding> findings;
    for (const Message &message : messages)
    {
        Checker(ts, message.name, findings, coverage).checkMessage(json::parse(message.text), message.dataInterface);
    }
    return findings;
}

/// Each optional field of an object type the messages were compared with that none of them has. A field counts once,
/// where it is declared, whichever interface a message is compared with: FX's execLegType, which ExecutionReport
/// declares and every report class inherits, is written in a swap leg's trade report alone.
std::vector<Finding> unwritten(const TsDeclarations &ts, const Coverage &coverage)
{
    std::vector<Finding> findings;
    std::set<const TsField *> named;
    for (const auto &[object, display] : coverage.compared)
    {
        for (const TsField *field : fieldsOf(ts, *object))
        {
            if (field->optional && (0 == coverage.written.count(field)) && named.insert(field).second)
            {
                const std::string declared = field->name + "?: " + describe(*field->type) + " (" + field->where + ")";
                findings.push_back(Finding{ Problem::UNCHECKED, display + " declares " + declared +
                                                                    ", which no message writes, so nothing compares "
                                                                    "it: give it to one of the test's messages" });
            }
        }
    }
    std::sort(findings.begin(), findings.end(),
              [](const Finding &a, const Finding &b)
              {
                  return a.text < b.text;
              });
    return findings;
}

/// The first group of each match
std::set<std::string> matches(const std::string &text, const std::regex &pattern)
{
    std::set<std::string> found;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; end != it; ++it)
    {
        found.insert((*it)[1].str());
    }
    return found;
}

std::set<std::string> typesOf(const std::vector<Message> &messages)
{
    std::set<std::string> types;
    for (const Message &message : messages)
    {
        types.insert(json::parse(message.text).value("type", ""));
    }
    return types;
}

// ---------------------------------------------------------------------------------------------------------------------
// What the server sends

const DateTimeT DAY_START = 1792098000000ULL; // a business day's start, 17:00 New York, in ms since the epoch
const DateTimeT DAY_MS = 24ULL * 60 * 60 * 1000;

class WireContractTest : public test::OrderStorageFixture
{
protected:
    /// A message of each type the server sends, and an execution report of each class (#179), each written by the
    /// server's serializer with every field it can carry: each list has an entry, the order has every reference set,
    /// and on FX a swap and its legs' reports carry what the serializer writes for a swap alone
    std::vector<Message> serverMessages();
};

std::vector<Message> WireContractTest::serverMessages()
{
    WideParamsDataStorage *wide = WideDataStorage::instance();
    const SourceIdT instrument = test::addInstrument("AAPL", "US0378331005", "ISIN");
    const SourceIdT account = test::addAccount("TRADING-1", "Apex Capital", AGENCY_ACCOUNTTYPE);
    const SourceIdT pair = test::addInstrument("EURUSD", "EURUSD", "FX"); // the swap's
    std::vector<Message> messages{
        { "connected", App::serializeConnected(), "", "" },
        { "instrument_list", App::serializeInstrumentList(wide), "", "" },
        { "account_list", App::serializeAccountList(wide), "", "" },
    };

    // A replacement, with an account, so that every reference the order names is set
    const auto rawData = [wide](const char *text)
    {
        return wide->add(new RawDataEntry(STRING_RAWDATATYPE, text, static_cast<u32>(std::strlen(text))));
    };
    auto order =
        std::make_unique<OrderEntry>(wide->add(new StringT("WebSocket")), wide->add(new StringT("Internal")),
                                     rawData("CL-2"), rawData("CL-1"), instrument, account, SourceIdT(), SourceIdT());
    order->orderId_ = IdT(2, 1);
    order->origOrderId_ = IdT(1, 1);
    order->side_ = BUY_SIDE;
    order->ordType_ = LIMIT_ORDERTYPE;
    order->tif_ = DAY_TIF;
    order->capacity_ = AGENCY_CAPACITY;
    order->currency_ = USD_CURRENCY;
    order->status_ = PARTFILL_ORDSTATUS;
    order->price_ = 10.5;
    order->orderQty_ = 100;
    order->cumQty_ = 30;
    order->leavesQty_ = 70;
    order->avgPx_ = 10.25;
    order->dayOrderQty_ = 100;
    order->dayCumQty_ = 30;
    order->dayAvgPx_ = 10.25;
    order->dayStart_ = DAY_START;
    order->creationTime_ = DAY_START + 1000;
    order->lastUpdateTime_ = DAY_START + 2000;
    // With the figures a report hands it (#411), as WsOutQueues sends it
    messages.push_back({ "order_update", App::serializeOrderUpdate(*order, OrderFigures(*order)), "", "" });
    // On FX, a swap, partly filled: the serializer writes its settlement dates and its far leg's price for a swap
    // alone, in the update and in the snapshot
    auto swap = std::make_unique<OrderEntry>(wide->add(new StringT("WebSocket")), wide->add(new StringT("Internal")),
                                             rawData("CL-3"), SourceIdT(), pair, account, SourceIdT(), SourceIdT());
    swap->orderId_ = IdT(5, 1);
    swap->side_ = SELL_SIDE;
    swap->ordType_ = FXSWAP_ORDERTYPE;
    swap->tif_ = GTC_TIF;
    swap->capacity_ = PRINCIPAL_CAPACITY;
    swap->currency_ = EUR_CURRENCY;
    swap->status_ = PARTFILL_ORDSTATUS;
    swap->price_ = 1.085; // the near leg's
    swap->farPrice_ = 1.0872;
    swap->settlDate_ = (DAY_START / DAY_MS + 3) * DAY_MS; // spot, at midnight UTC, as the engine keeps one (#198)
    swap->farSettlDate_ = swap->settlDate_ + 30 * DAY_MS;
    swap->orderQty_ = 1000000;
    swap->cumQty_ = 400000;
    swap->leavesQty_ = 600000;
    swap->avgPx_ = 1.085;
    swap->dayOrderQty_ = 1000000;
    swap->dayCumQty_ = 400000;
    swap->dayAvgPx_ = 1.085;
    swap->dayStart_ = DAY_START;
    swap->creationTime_ = DAY_START + 1000;
    swap->lastUpdateTime_ = DAY_START + 2000;
    messages.push_back({ "order_update (FXSWAP)", App::serializeOrderUpdate(*swap, OrderFigures(*swap)), "", "" });
    // Restored, so published (#404): the snapshot leaves out an order whose first report isn't out
    OrderStorage::instance()->restore(order.release());
    OrderStorage::instance()->restore(swap.release());
    messages.push_back({ "order_snapshot", App::serializeOrderSnapshot(OrderStorage::instance()), "", "" });

    // Every field of each report is set: ExecCorrectParams has no constructor
    ExecutionEntry orderCancel; // a plain report, of type CANCEL (#179)
    TradeExecEntry trade;
    trade.lastQty_ = 30;
    trade.lastPx_ = 10.25;
    trade.currency_ = USD_CURRENCY;
    trade.tradeDate_ = 20261124;
    RejectExecEntry refusedOrder; // refused before it was stored, so with no order id, as WsOutQueues makes it (#67)
    refusedOrder.rejectReason_ = "Order refused: no market";
    RejectExecEntry refusedReplace; // of type REPLACE (#179)
    refusedReplace.rejectReason_ = "Replace refused: the order is filled";
    ReplaceExecEntry replacement;
    replacement.origOrderId_ = IdT(1, 1);
    ExecCorrectExecEntry correction{};
    correction.cumQty_ = 30;
    correction.leavesQty_ = 70;
    correction.lastQty_ = 30;
    correction.lastPx_ = 10.2;
    correction.currency_ = USD_CURRENCY;
    correction.tradeDate_ = 20261124;
    correction.origOrderId_ = IdT(1, 1);
    correction.execRefId_ = IdT(4, 1);
    TradeCancelExecEntry tradeCancel;
    tradeCancel.execRefId_ = IdT(4, 1);
    // On FX, a swap's fill reports each of its legs as a trade, naming the leg (#153), which the serializer writes for
    // a swap leg alone
    TradeExecEntry nearLeg;
    nearLeg.lastQty_ = 400000;
    nearLeg.lastPx_ = 1.085;
    nearLeg.currency_ = EUR_CURRENCY;
    nearLeg.tradeDate_ = 20261124;
    nearLeg.execLegType_ = NEAR_LEG;
    TradeExecEntry farLeg(nearLeg);
    farLeg.lastPx_ = 1.0872;
    farLeg.execLegType_ = FAR_LEG;
    struct Report
    {
        ExecutionEntry *exec;
        const char *cppClass;
        ExecType type;
        OrderStatus status;
        IdT orderId;
    };
    for (const Report &report : {
             Report{ &orderCancel, "ExecutionEntry", CANCEL_EXECTYPE, CANCELED_ORDSTATUS, IdT(2, 1) },
             Report{ &trade, "TradeExecEntry", TRADE_EXECTYPE, PARTFILL_ORDSTATUS, IdT(2, 1) },
             Report{ &refusedOrder, "RejectExecEntry", REJECT_EXECTYPE, REJECTED_ORDSTATUS, IdT() },
             Report{ &refusedReplace, "RejectExecEntry", REPLACE_EXECTYPE, FILLED_ORDSTATUS, IdT(2, 1) },
             Report{ &replacement, "ReplaceExecEntry", REPLACE_EXECTYPE, NEW_ORDSTATUS, IdT(2, 1) },
             Report{ &correction, "ExecCorrectExecEntry", CORRECT_EXECTYPE, PARTFILL_ORDSTATUS, IdT(2, 1) },
             Report{ &tradeCancel, "TradeCancelExecEntry", CANCEL_EXECTYPE, NEW_ORDSTATUS, IdT(2, 1) },
             Report{ &nearLeg, "TradeExecEntry", TRADE_EXECTYPE, PARTFILL_ORDSTATUS, IdT(5, 1) },
             Report{ &farLeg, "TradeExecEntry", TRADE_EXECTYPE, PARTFILL_ORDSTATUS, IdT(5, 1) },
         })
    {
        report.exec->type_ = report.type;
        report.exec->orderStatus_ = report.status;
        report.exec->orderId_ = report.orderId;
        report.exec->execId_ = IdT(3, 1);
        report.exec->transactTime_ = DAY_START + 3000;
        report.exec->market_ = INTERNAL_EXECUTION;
        const char *const leg = (NEAR_LEG == report.exec->execLegType_)  ? ", NEAR leg"
                                : (FAR_LEG == report.exec->execLegType_) ? ", FAR leg"
                                                                         : "";
        messages.push_back({ "execution_report (" + std::string(report.cppClass) + ", " +
                                 std::string(toJsonString(report.type)) + leg + ")",
                             App::serializeExecReport(report.exec), report.cppClass,
                             REPORT_INTERFACES.at(report.cppClass) });
    }

    BookSnapshot book;
    book.bids.push_back(BookLevel{ 10.25, 100, 2 });
    book.asks.push_back(BookLevel{ 10.5, 50, 1 });
    messages.push_back({ "book_update", App::serializeBookUpdate("AAPL", book), "", "" });
    messages.push_back(
        { "cancel_reject", App::serializeCancelReject(2, "Cancel rejected: too late, the order is FILLED"), "", "" });
    messages.push_back({ "error", App::serializeError("Unknown message type: hello"), "", "" });
    // Each field is written whatever its value
    messages.push_back({ "metrics_update", App::serializeMetricsUpdate(App::SystemMetrics{}), "", "" });
    return messages;
}

TEST_F(WireContractTest, EveryFieldTheServerWritesIsOneTheFrontendDeclares)
{
    // With a value of the type declared: a literal type's text exactly, as each message's type and each report's are
    const TsDeclarations ts = readFrontend();
    for (const Finding &finding : check(ts, serverMessages()))
    {
        if (Problem::MISSING != finding.problem)
        {
            ADD_FAILURE() << finding.text;
        }
    }
}

TEST_F(WireContractTest, EveryFieldTheFrontendRequiresIsOneTheServerWrites)
{
    // A field the frontend declares optional (?:) may be absent
    const TsDeclarations ts = readFrontend();
    for (const Finding &finding : check(ts, serverMessages()))
    {
        if ((Problem::MISSING == finding.problem) || (Problem::UNCHECKED == finding.problem))
        {
            ADD_FAILURE() << finding.text;
        }
    }
}

TEST_F(WireContractTest, EveryOptionalFieldTheFrontendDeclaresIsOneAMessageWrites)
{
    // But not absent from every message, or nothing compares it: on FX, the swap's fields and its legs' execLegType,
    // which the server writes for a swap alone, so that a swap and its legs' reports are among the messages
    const TsDeclarations ts = readFrontend();
    Coverage coverage;
    check(ts, serverMessages(), &coverage); // what the messages have wrong, the cases above report
    for (const Finding &finding : unwritten(ts, coverage))
    {
        ADD_FAILURE() << finding.text;
    }
}

TEST_F(WireContractTest, EveryMessageTheFrontendHandlesIsOneTheServerSends)
{
    // A variant the server doesn't send is a message the frontend waits for in vain, as business_reject was (#521)
    const TsDeclarations ts = readFrontend();
    const std::set<std::string> sent = typesOf(serverMessages());
    for (const auto &[type, variant] : serverVariants(ts))
    {
        EXPECT_EQ(1u, sent.count(type)) << "ServerMessage's '" << type << "' variant (" << variant->where
                                        << ") is a message the server doesn't send, or one serverMessages() lacks";
    }
}

TEST_F(WireContractTest, EveryMessageAndReportClassTheSerializerWritesIsOneThisTestChecks)
{
    // Read from the serializer's source, so that a message or a report class added there fails here until
    // serverMessages() writes it, rather than going unchecked (#638). Both ways: a serializer written so that this scan
    // finds nothing fails too, rather than passing.
    const std::string source = readFile(test::sourceRoot() / "app/JsonSerializer.cpp");
    const std::set<std::string> written = matches(source, std::regex(R"re(\["type"\]\s*=\s*"(\w+)")re"));
    std::set<std::string> classes = matches(source, std::regex(R"re(dynamic_cast<\s*const\s+(\w+)\s*\*\s*>)re"));
    classes.insert("ExecutionEntry"); // what a report of none of those classes is written as
    const std::vector<Message> messages = serverMessages();
    const std::set<std::string> sent = typesOf(messages);
    std::set<std::string> built;
    for (const Message &message : messages)
    {
        if (!message.reportClass.empty())
        {
            built.insert(message.reportClass);
        }
    }
    for (const std::string &type : written)
    {
        EXPECT_EQ(1u, sent.count(type)) << "app/JsonSerializer.cpp writes a message of type '" << type
                                        << "', which serverMessages() doesn't, so nothing checks it";
    }
    for (const std::string &type : sent)
    {
        EXPECT_EQ(1u, written.count(type)) << "serverMessages() writes a message of type '" << type << "', which this "
                                           << "test's scan of app/JsonSerializer.cpp for [\"type\"] = \"...\" misses";
    }
    for (const std::string &cppClass : classes)
    {
        EXPECT_EQ(1u, built.count(cppClass)) << "app/JsonSerializer.cpp writes a report of class " << cppClass
                                             << ", which serverMessages() doesn't, so nothing checks it";
        EXPECT_EQ(1u, REPORT_INTERFACES.count(cppClass))
            << "REPORT_INTERFACES gives no interface for " << cppClass << ", a class app/JsonSerializer.cpp writes";
    }
    for (const auto &[cppClass, interface] : REPORT_INTERFACES)
    {
        EXPECT_EQ(1u, classes.count(cppClass)) << "REPORT_INTERFACES names " << cppClass << ", which this test's scan "
                                               << "of app/JsonSerializer.cpp for dynamic_cast<const X *> misses";
    }
}

TEST_F(WireContractTest, EachReportClassHasItsInterfaceInTheFrontendsExecution)
{
    // The frontend reads an execution report as an Execution, and tells its classes apart with index.ts's guards
    const TsDeclarations ts = readFrontend();
    const auto execution = ts.aliases.find("Execution");
    ASSERT_NE(ts.aliases.end(), execution) << "index.ts declares no Execution";
    const TsType &type = *execution->second;
    std::set<std::string> alternatives;
    for (const TsTypeP &alternative :
         (TsType::UNION == type.kind) ? type.of : std::vector<TsTypeP>{ execution->second })
    {
        alternatives.insert(describe(*alternative));
    }
    std::set<std::string> interfaces;
    for (const auto &[cppClass, interface] : REPORT_INTERFACES)
    {
        interfaces.insert(interface);
        EXPECT_EQ(1u, alternatives.count(interface))
            << cppClass << " is read as " << interface << ", which isn't one of Execution's (" << type.where << ")";
    }
    for (const std::string &alternative : alternatives)
    {
        EXPECT_EQ(1u, interfaces.count(alternative))
            << "Execution (" << type.where << ") takes " << alternative
            << ", which is no class of report the server writes: REPORT_INTERFACES names none";
    }
}

TEST_F(WireContractTest, TheCheckReadsEachFieldAsItsTypeDeclaresIt)
{
    // On types of its own, since today's don't exercise everything
    TsDeclarations ts;
    TsParser("snippet.ts",
             "export enum Kind { A = 'A', B = 'B', }\n"
             "export interface Base { id: number; kind: Kind; }\n"
             "export interface Item extends Base { kind: Kind.B; name: string; note?: string; }\n"
             "export interface Other { id: number; }\n"
             "export type ServerMessage = | { type: 'base'; data: Base } | { type: 'list'; data: Base[] };\n",
             ts)
        .parse();
    validate(ts);
    const struct
    {
        const char *message;
        const char *dataInterface;
        Problem problem; // of the one finding expected, if text isn't empty
        const char *text;
    } cases[] = {
        { R"({"type":"base","data":{"id":1,"kind":"A"}})", "", Problem::MISSING, "" },
        { R"({"type":"base","data":{"id":1,"kind":"B","name":"n"}})", "Item", Problem::MISSING, "" },
        { R"({"type":"base","data":{"id":1,"kind":"B","name":"n","note":"x"}})", "Item", Problem::MISSING, "" },
        { R"({"type":"base","data":{"id":1,"kind":"B","name":"n","note":3}})", "Item", Problem::MISTYPED,
          "the server writes data.note as a number, where Item (snippet.ts:3) declares note: string (snippet.ts:3)" },
        { R"({"type":"base","data":{"id":1,"kind":"B"}})", "Item", Problem::MISSING,
          "Item (snippet.ts:3) requires name (snippet.ts:3), which the server doesn't write in data" },
        { R"({"type":"base","data":{"kind":"B","name":"n"}})", "Item", Problem::MISSING,
          "Item (snippet.ts:3) requires id (snippet.ts:2)" },
        { R"({"type":"base","data":{"id":1,"kind":"A","name":"n"}})", "Item", Problem::MISTYPED,
          "data.kind as \"A\", where Item (snippet.ts:3) declares kind: Kind.B" },
        { R"({"type":"base","data":{"id":1,"kind":"B","name":"n","size":2}})", "Item", Problem::UNDECLARED,
          "the server writes data.size, which Item (snippet.ts:3) doesn't declare" },
        { R"({"type":"base","data":{"id":1}})", "Other", Problem::MISTYPED,
          "declares data: Base, which Other neither is nor extends" },
        { R"({"type":"base","data":{"id":"1","kind":"A"}})", "", Problem::MISTYPED,
          "data.id as a string, where Base (snippet.ts:2) declares id: number" },
        { R"({"type":"list","data":[]})", "", Problem::UNCHECKED, "data is empty" },
        { R"({"type":"list","data":[{"id":1}]})", "", Problem::MISSING,
          "requires kind (snippet.ts:2), which the "
          "server doesn't write in data[0]" },
        { R"({"type":"base"})", "", Problem::MISSING, "ServerMessage's 'base' variant (snippet.ts:5) requires data" },
        { R"({"type":"none"})", "", Problem::UNDECLARED, "type 'none', which ServerMessage (snippet.ts:5) has no" },
    };
    for (const auto &c : cases)
    {
        SCOPED_TRACE(c.message);
        const std::vector<Finding> found = check(ts, { Message{ "test", c.message, "", c.dataInterface } });
        const size_t expected = (0 == std::strlen(c.text)) ? 0 : 1;
        EXPECT_EQ(expected, found.size()) << (found.empty() ? "" : found[0].text);
        if ((1 == expected) && (1 == found.size()))
        {
            EXPECT_EQ(c.problem, found[0].problem) << found[0].text;
            EXPECT_NE(std::string::npos, found[0].text.find(c.text)) << found[0].text;
        }
    }
}

TEST_F(WireContractTest, TheCheckFindsEachOptionalFieldNoMessageWrites)
{
    // Across the messages, each field counted where it is declared: leg here is as FX's execLegType, which
    // ExecutionReport declares, every report class inherits, and a swap leg's trade report alone writes
    TsDeclarations ts;
    TsParser("snippet.ts",
             "export interface Base { id: number; leg?: 'NEAR' | 'FAR'; }\n"
             "export interface Trade extends Base { qty: number; }\n"
             "export interface Reject extends Base { reason: string; note?: string; }\n"
             "export type ServerMessage = { type: 'report'; data: Base };\n",
             ts)
        .parse();
    validate(ts);
    const Message base{ "base", R"({"type":"report","data":{"id":1}})", "", "" };
    const Message leg{ "leg", R"({"type":"report","data":{"id":1,"leg":"FAR","qty":5}})", "", "Trade" };
    const Message reject{ "reject", R"({"type":"report","data":{"id":1,"reason":"r"}})", "", "Reject" };
    const Message noted{ "noted", R"({"type":"report","data":{"id":1,"reason":"r","note":"n"}})", "", "Reject" };
    const struct
    {
        std::vector<Message> messages;
        std::vector<std::string> unwritten; // what each finding starts with, in order
    } cases[] = {
        { { base }, { "Base (snippet.ts:1) declares leg?: 'NEAR' | 'FAR' (snippet.ts:1), which no message writes" } },
        { { base, leg }, {} },
        { { reject },
          { "Reject (snippet.ts:3) declares leg?: 'NEAR' | 'FAR' (snippet.ts:1)",
            "Reject (snippet.ts:3) declares note?: string (snippet.ts:3)" } },
        { { leg, reject }, { "Reject (snippet.ts:3) declares note?: string (snippet.ts:3)" } },
        { { leg, noted }, {} },
    };
    for (const auto &c : cases)
    {
        std::string names;
        for (const Message &message : c.messages)
        {
            names += message.name + " ";
        }
        SCOPED_TRACE(names);
        Coverage coverage;
        const std::vector<Finding> wrong = check(ts, c.messages, &coverage);
        EXPECT_TRUE(wrong.empty()) << (wrong.empty() ? "" : wrong[0].text);
        const std::vector<Finding> found = unwritten(ts, coverage);
        ASSERT_EQ(c.unwritten.size(), found.size()) << (found.empty() ? "" : found[0].text);
        for (size_t i = 0; i < found.size(); ++i)
        {
            EXPECT_EQ(Problem::UNCHECKED, found[i].problem) << found[i].text;
            EXPECT_EQ(0u, found[i].text.find(c.unwritten[i])) << found[i].text;
        }
    }
}

TEST_F(WireContractTest, TheParserRefusesEachConstructItDoesNotKnow)
{
    // Each refusal names the construct and the declaration and field it is in: read as something else, a construct
    // could let a field the frontend doesn't declare pass (#638)
    const struct
    {
        const char *text;
        const char *refusal;
    } cases[] = {
        { "export interface X { readonly a: number; }", "interface X, field a: unsupported construct: the modifier" },
        { "export interface X { [key: string]: number; }", "interface X: unsupported construct: an index signature" },
        { "export interface X { a: Array<number>; }", "interface X, field a: unsupported construct: generic type" },
        { "export interface X { a: number | null; }", "interface X, field a: unsupported construct: the type 'null'" },
        { "export interface X { a(): number; }", "interface X, field a: unsupported construct: a method signature" },
        { "export interface X { a: number, b: number }", "interface X, field a: unsupported construct: ','" },
        { "export interface X extends Y, Z { a: number; }", "interface X: unsupported construct: an interface that" },
        { "export type X = { a: number } & { b: number };", "type X: unsupported construct: an intersection type" },
        { "export enum X { A = 1, }", "enum X, member A: unsupported construct: an enum member whose value isn't" },
        { "export const X = 1;", "unsupported construct: an export of 'const'" },
        { "export function X(a: number): boolean { return true; }", "function X: unsupported construct: a return" },
        { "export type X = `a`;", "unsupported construct: a template literal" },
        { "interface X { a: number; }", "unsupported construct: a top-level 'interface'" },
    };
    for (const auto &c : cases)
    {
        TsDeclarations ts;
        try
        {
            TsParser("snippet.ts", c.text, ts).parse();
            ADD_FAILURE() << "read without a refusal: " << c.text;
        }
        catch (const std::runtime_error &e)
        {
            EXPECT_NE(std::string::npos, std::string(e.what()).find(c.refusal))
                << c.text << "\nrefused as: " << e.what();
        }
    }
}

} // namespace
