#include <optional>
#include <unordered_map>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>
#include <vector>
#include "Luau/Ast.h"
#include "Luau/LuauConfig.h"
#include "Luau/Parser.h"
#include "LSP/WorkspaceFileResolver.hpp"

#include "Luau/TimeTrace.h"
#include "LuauFileUtils.hpp"

#include "Plugin/PluginManager.hpp"
#include "Plugin/PluginTextDocument.hpp"
#include "Plugin/SourceMapping.hpp"

#include "lua.h"

struct LuauConfigInterruptInfo
{
    Luau::TypeCheckLimits limits;
    std::string module;
};

namespace
{
enum class MTAScriptType
{
    Unknown,
    Client,
    Server,
    Shared,
};

static std::string normalizePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });
    return path;
}

static MTAScriptType parseMtaScriptType(const std::string& scriptType)
{
    std::string lowered = scriptType;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

    if (lowered == "client")
        return MTAScriptType::Client;
    if (lowered == "shared")
        return MTAScriptType::Shared;
    if (lowered == "server" || lowered.empty())
        return MTAScriptType::Server;

    return MTAScriptType::Unknown;
}

static std::optional<Uri> findResourceMetaFile(Uri uri)
{
    if (uri.scheme != "file")
        return std::nullopt;

    std::optional<Uri> current = uri.parent();
    while (current)
    {
        auto meta = current->resolvePath("meta.xml");
        if (meta.exists())
            return meta;
        current = current->parent();
    }

    return std::nullopt;
}

static std::optional<size_t> offsetFromPosition(const std::string& source, const Luau::Position& pos)
{
    size_t line = 0;
    size_t column = 0;

    for (size_t i = 0; i < source.size(); ++i)
    {
        if (line == pos.line && column == pos.column)
            return i;

        char ch = source[i];
        if (ch == '\r')
        {
            if (i + 1 < source.size() && source[i + 1] == '\n')
                ++i;
            ++line;
            column = 0;
        }
        else if (ch == '\n')
        {
            ++line;
            column = 0;
        }
        else
            ++column;
    }

    if (line == pos.line && column == pos.column)
        return source.size();

    return std::nullopt;
}

static std::optional<std::string> sourceSliceFromLocation(const std::string& source, const Luau::Location& location)
{
    auto start = offsetFromPosition(source, location.begin);
    auto end = offsetFromPosition(source, location.end);
    if (!start || !end || *start > *end || *end > source.size())
        return std::nullopt;

    return source.substr(*start, *end - *start);
}

static std::string trimLeadingWhitespace(std::string value)
{
    size_t i = 0;
    while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i])))
        ++i;

    if (i == 0)
        return value;

    return value.substr(i);
}

static std::optional<std::string> normalizeFunctionExpressionSlice(std::string functionSlice)
{
    functionSlice = trimLeadingWhitespace(std::move(functionSlice));

    if (functionSlice.rfind("function(", 0) == 0)
        return functionSlice;

    if (functionSlice.rfind("function ", 0) != 0)
        return std::nullopt;

    size_t openParen = functionSlice.find('(');
    if (openParen == std::string::npos)
        return std::nullopt;

    return std::string("function") + functionSlice.substr(openParen);
}

static std::optional<std::string> sourceSliceForType(const std::string& source, const Luau::AstType* type)
{
    if (!type)
        return std::nullopt;

    return sourceSliceFromLocation(source, type->location);
}

static std::optional<std::string> sourceSliceForTypePack(const std::string& source, const Luau::AstTypePack* typePack)
{
    if (!typePack)
        return std::nullopt;

    return sourceSliceFromLocation(source, typePack->location);
}

static std::string safeNameOr(const Luau::AstName& name, const std::string& fallback)
{
    if (name.value)
        return std::string(name.value);

    return fallback;
}

static std::string buildFunctionTypeFromAst(const std::string& source, const Luau::AstExprFunction* func)
{
    std::string type = "(";
    bool first = true;

    auto appendArg = [&](const std::string& name, const std::string& argType)
    {
        if (!first)
            type += ", ";

        type += name;
        type += ": ";
        type += argType;
        first = false;
    };

    if (func->self)
    {
        std::string selfType = "any";
        if (auto selfAnn = sourceSliceForType(source, func->self->annotation))
            selfType = *selfAnn;

        appendArg("self", selfType);
    }

    for (size_t i = 0; i < func->args.size; ++i)
    {
        auto* arg = func->args.data[i];
        std::string argType = "any";
        if (auto argAnn = sourceSliceForType(source, arg->annotation))
            argType = *argAnn;

        appendArg(safeNameOr(arg->name, "arg" + std::to_string(i)), argType);
    }

    if (func->vararg)
    {
        std::string varargType = "...any";
        if (auto varargAnn = sourceSliceForTypePack(source, func->varargAnnotation))
            varargType = "..." + *varargAnn;

        if (!first)
            type += ", ";
        type += varargType;
        first = false;
    }

    type += ") -> ";

    if (auto returnAnn = sourceSliceForTypePack(source, func->returnAnnotation))
        type += *returnAnn;
    else
        type += "any";

    return type;
}

static std::optional<std::string> astNameToString(const Luau::AstName& name);
static bool isIdentifier(const std::string& value);

static std::unordered_set<std::string> extractTopLevelGlobalFunctionNames(const std::string& source)
{
    std::unordered_set<std::string> names;

    Luau::Allocator allocator;
    Luau::AstNameTable nameTable{allocator};
    Luau::ParseOptions options;
    Luau::ParseResult parseResult = Luau::Parser::parse(source.c_str(), source.size(), nameTable, allocator, options);
    if (!parseResult.root)
        return names;

    for (Luau::AstStat* stat : parseResult.root->body)
    {
        auto* fn = stat->as<Luau::AstStatFunction>();
        if (!fn)
            continue;

        auto* global = fn->name->as<Luau::AstExprGlobal>();
        if (!global)
            continue;

        if (auto name = astNameToString(global->name); name && isIdentifier(*name))
            names.insert(*name);
    }

    return names;
}

struct GlobalNameReferenceCollector : Luau::AstVisitor
{
    const std::unordered_set<std::string>& tracked;
    std::unordered_set<std::string> referenced;

    explicit GlobalNameReferenceCollector(const std::unordered_set<std::string>& tracked)
        : tracked(tracked)
    {
    }

    bool visit(Luau::AstExprGlobal* global) override
    {
        if (auto name = astNameToString(global->name); name && tracked.count(*name))
            referenced.insert(*name);

        return true;
    }

    bool visit(Luau::AstStatFunction* fn) override
    {
        if (fn->func)
            fn->func->visit(this);
        return false;
    }
};

static std::unordered_set<std::string> extractReferencedGlobalNames(
    const std::string& source, const std::unordered_set<std::string>& trackedNames)
{
    std::unordered_set<std::string> referenced;

    if (trackedNames.empty())
        return referenced;

    Luau::Allocator allocator;
    Luau::AstNameTable nameTable{allocator};
    Luau::ParseOptions options;
    Luau::ParseResult parseResult = Luau::Parser::parse(source.c_str(), source.size(), nameTable, allocator, options);
    if (!parseResult.root)
        return referenced;

    GlobalNameReferenceCollector collector{trackedNames};
    parseResult.root->visit(&collector);
    return std::move(collector.referenced);
}

static std::optional<std::string> buildUsedGlobalFunctionsPrelude(
    const std::unordered_set<std::string>& definedFunctions, const std::unordered_set<std::string>& referencedFunctions)
{
    std::vector<std::string> usedFunctions;
    usedFunctions.reserve(definedFunctions.size());
    for (const auto& name : definedFunctions)
    {
        if (referencedFunctions.count(name))
            usedFunctions.push_back(name);
    }

    if (usedFunctions.empty())
        return std::nullopt;

    std::sort(usedFunctions.begin(), usedFunctions.end());

    std::string prelude;
    prelude.reserve(usedFunctions.size() * 48);
    prelude += "\n--!nolint\n";

    for (size_t i = 0; i < usedFunctions.size(); ++i)
    {
        prelude += "if false then (";
        prelude += usedFunctions[i];
        prelude += " :: any)() end\n";
    }

    return prelude;
}

static bool hasLikelyGlobalFunctionCall(const std::string& source, const std::string& functionName)
{
    size_t pos = 0;
    while ((pos = source.find(functionName, pos)) != std::string::npos)
    {
        bool leftOk = (pos == 0) || !std::isalnum(static_cast<unsigned char>(source[pos - 1])) && source[pos - 1] != '_';
        if (!leftOk)
        {
            ++pos;
            continue;
        }

        size_t afterName = pos + functionName.size();
        bool rightBoundary = (afterName >= source.size()) ||
            (!std::isalnum(static_cast<unsigned char>(source[afterName])) && source[afterName] != '_');
        if (!rightBoundary)
        {
            ++pos;
            continue;
        }

        size_t i = afterName;
        while (i < source.size() && std::isspace(static_cast<unsigned char>(source[i])))
            ++i;

        if (i >= source.size() || source[i] != '(')
        {
            ++pos;
            continue;
        }

        size_t lineStart = source.rfind('\n', pos);
        if (lineStart == std::string::npos)
            lineStart = 0;
        else
            ++lineStart;

        std::string linePrefix = source.substr(lineStart, pos - lineStart);
        if (linePrefix.find("function") != std::string::npos)
        {
            ++pos;
            continue;
        }

        return true;
    }

    return false;
}

static Luau::Position endPosition(const std::string& source)
{
    Luau::Position pos{0, 0};
    for (size_t i = 0; i < source.size(); ++i)
    {
        char ch = source[i];
        if (ch == '\r')
        {
            if (i + 1 < source.size() && source[i + 1] == '\n')
                ++i;
            ++pos.line;
            pos.column = 0;
        }
        else if (ch == '\n')
        {
            ++pos.line;
            pos.column = 0;
        }
        else
            ++pos.column;
    }
    return pos;
}

static Luau::Position preludeInsertionPosition(const std::string& source)
{
    // Keep Luau mode directives (`--!strict`, etc.) at the beginning of file.
    // Insert declarations right after any leading directive lines.
    size_t index = 0;
    unsigned int line = 0;

    while (index < source.size())
    {
        size_t lineStart = index;
        while (index < source.size() && source[index] != '\n' && source[index] != '\r')
            ++index;

        size_t lineEnd = index;

        // Handle CRLF/CR/LF
        if (index < source.size())
        {
            if (source[index] == '\r' && index + 1 < source.size() && source[index + 1] == '\n')
                index += 2;
            else
                index += 1;
        }

        std::string lineText = source.substr(lineStart, lineEnd - lineStart);
        if (!(lineText.rfind("--!", 0) == 0))
            return Luau::Position{line, 0};

        ++line;
    }

    return Luau::Position{line, 0};
}

struct MtaScriptEntry
{
    Uri uri;
    MTAScriptType type = MTAScriptType::Unknown;
};

static std::optional<std::string> astNameToString(const Luau::AstName& name)
{
    if (!name.value)
        return std::nullopt;

    return std::string(name.value);
}

static std::optional<std::string> simpleExprName(const Luau::AstExpr* expr)
{
    if (const auto* local = expr->as<Luau::AstExprLocal>())
        return astNameToString(local->local->name);

    if (const auto* global = expr->as<Luau::AstExprGlobal>())
        return astNameToString(global->name);

    return std::nullopt;
}

static std::optional<std::string> constantStringValue(const Luau::AstExpr* expr)
{
    const auto* stringExpr = expr->as<Luau::AstExprConstantString>();
    if (!stringExpr || !stringExpr->value.data || stringExpr->value.size == 0)
        return std::nullopt;

    std::string value(stringExpr->value.data, stringExpr->value.size);
    if (!value.empty() && value.back() == '\0')
        value.pop_back();

    return value;
}

static std::optional<std::string> oopClassTypeNameFromCreateArg(const Luau::AstExpr* expr)
{
    if (auto directString = constantStringValue(expr))
        return directString;

    const auto* tableExpr = expr->as<Luau::AstExprTable>();
    if (!tableExpr)
        return std::nullopt;

    for (const auto& item : tableExpr->items)
    {
        if (item.kind != Luau::AstExprTable::Item::Record)
            continue;

        auto key = constantStringValue(item.key);
        if (!key || (*key != "type" && *key != "name"))
            continue;

        if (auto typeName = constantStringValue(item.value))
            return typeName;
    }

    return std::nullopt;
}

static bool isIdentifier(const std::string& value)
{
    if (value.empty())
        return false;

    auto isIdentifierStart = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };

    auto isIdentifierByte = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };

    if (!isIdentifierStart(value[0]))
        return false;

    for (size_t i = 1; i < value.size(); ++i)
    {
        if (!isIdentifierByte(value[i]))
            return false;
    }

    return true;
}

struct OopClassBinding
{
    std::string globalName;
    std::string localName;
    std::optional<std::string> explicitPublicType;
};

static const Luau::AstExpr* unwrapTypeAssertionExpr(const Luau::AstExpr* expr)
{
    if (const auto* asserted = expr->as<Luau::AstExprTypeAssertion>())
        return asserted->expr;

    return expr;
}

static std::optional<std::string> extractOopPublicTypeFromAssertion(const Luau::AstExpr* expr, const std::string& source)
{
    const auto* asserted = expr->as<Luau::AstExprTypeAssertion>();
    if (!asserted)
        return std::nullopt;

    const auto* annotationRef = asserted->annotation->as<Luau::AstTypeReference>();
    if (!annotationRef)
        return std::nullopt;

    auto annotationName = astNameToString(annotationRef->name);
    if (!annotationName || *annotationName != "OopDefinitionOf")
        return std::nullopt;

    if (annotationRef->parameters.size == 0 || !annotationRef->parameters.data[0].type)
        return std::nullopt;

    return sourceSliceFromLocation(source, annotationRef->parameters.data[0].type->location);
}

static std::optional<std::string> oopGlobalNameFromCreateCall(const Luau::AstExpr* expr)
{
    expr = unwrapTypeAssertionExpr(expr);

    const auto* call = expr->as<Luau::AstExprCall>();
    if (!call || !call->self || call->args.size == 0)
        return std::nullopt;

    const auto* member = call->func->as<Luau::AstExprIndexName>();
    if (!member)
        return std::nullopt;

    auto memberName = astNameToString(member->index);
    if (!memberName || (*memberName != "create" && *memberName != "define" && *memberName != "extend"))
        return std::nullopt;

    auto ownerName = simpleExprName(member->expr);
    if (!ownerName || *ownerName != "oop")
        return std::nullopt;

    return oopClassTypeNameFromCreateArg(call->args.data[0]);
}

static std::optional<std::pair<std::string, std::string>> classPublicMethodName(const Luau::AstExpr* expr)
{
    const auto* methodExpr = expr->as<Luau::AstExprIndexName>();
    if (!methodExpr)
        return std::nullopt;

    const auto* publicExpr = methodExpr->expr->as<Luau::AstExprIndexName>();
    if (!publicExpr || publicExpr->op != '.')
        return std::nullopt;

    auto publicName = astNameToString(publicExpr->index);
    if (!publicName || *publicName != "public")
        return std::nullopt;

    auto className = simpleExprName(publicExpr->expr);
    auto methodName = astNameToString(methodExpr->index);
    if (!className || !methodName)
        return std::nullopt;

    return std::make_pair(*className, *methodName);
}

static std::vector<MtaScriptEntry> parseMtaScriptEntries(const Uri& metaUri, const std::string& metaSource)
{
    std::vector<MtaScriptEntry> entries;
    const auto parent = metaUri.parent();
    if (!parent)
        return entries;

    static const std::regex scriptTagRegex(R"(<\s*script\b([^>]*)>)", std::regex::icase);
    static const std::regex attrRegex(R"mta(([A-Za-z0-9_:\-]+)\s*=\s*("([^"]*)"|'([^']*)'))mta", std::regex::icase);

    auto it = std::sregex_iterator(metaSource.begin(), metaSource.end(), scriptTagRegex);
    auto end = std::sregex_iterator();
    for (; it != end; ++it)
    {
        std::string attrs = (*it)[1].str();
        std::string src;
        std::string type = "server";

        auto attrIt = std::sregex_iterator(attrs.begin(), attrs.end(), attrRegex);
        for (; attrIt != end; ++attrIt)
        {
            auto key = (*attrIt)[1].str();
            auto value = (*attrIt)[3].matched ? (*attrIt)[3].str() : (*attrIt)[4].str();

            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });

            if (key == "src")
                src = value;
            else if (key == "type")
                type = value;
        }

        if (src.empty())
            continue;

        entries.push_back(MtaScriptEntry{parent->resolvePath(src), parseMtaScriptType(type)});
    }

    return entries;
}

static std::optional<std::string> extractTypedGlobalsPrelude(
    const std::string& source, const std::string& providerTag, bool emitSharedTypeAliases)
{
    auto normalizeTypeAliasForLocalScope = [](std::string alias) {
        size_t i = 0;
        while (i < alias.size() && std::isspace(static_cast<unsigned char>(alias[i])))
            ++i;

        static constexpr const char* exportType = "export type";
        if (alias.compare(i, std::char_traits<char>::length(exportType), exportType) == 0)
            alias.erase(i, std::char_traits<char>::length("export "));

        return alias;
    };

    std::string parseSource = source;
    size_t parseIndex = 0;
    while ((parseIndex = parseSource.find("export type", parseIndex)) != std::string::npos)
    {
        // Replace only the `export ` prefix with spaces to preserve source offsets.
        parseSource.replace(parseIndex, 7, "       ");
        parseIndex += 11;
    }

    Luau::Allocator allocator;
    Luau::AstNameTable names{allocator};
    Luau::ParseOptions options;
    Luau::ParseResult parseResult = Luau::Parser::parse(parseSource.c_str(), parseSource.size(), names, allocator, options);

    if (!parseResult.root)
        return std::nullopt;

    std::vector<std::string> typeAliases;
    std::vector<std::pair<std::string, std::string>> sharedTypeAliases;
    std::vector<std::pair<std::string, std::string>> globalTypeBodies;
    std::vector<OopClassBinding> oopClassBindings;
    std::unordered_map<std::string, std::string> localToOopGlobal;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> oopPublicMethodsByGlobal;

    auto addOopClassBinding =
        [&](std::string globalName, std::string localName, std::optional<std::string> explicitPublicType)
    {
        if (!isIdentifier(globalName) || !isIdentifier(localName))
            return;

        auto existingLocal = localToOopGlobal.find(localName);
        if (existingLocal != localToOopGlobal.end() && existingLocal->second == globalName)
        {
            for (auto& binding : oopClassBindings)
            {
                if (binding.localName == localName && binding.globalName == globalName && explicitPublicType)
                {
                    binding.explicitPublicType = std::move(explicitPublicType);
                    break;
                }
            }
            return;
        }

        localToOopGlobal[localName] = globalName;
        oopClassBindings.push_back({std::move(globalName), std::move(localName), std::move(explicitPublicType)});
    };

    for (Luau::AstStat* stat : parseResult.root->body)
    {
        if (auto* alias = stat->as<Luau::AstStatTypeAlias>())
        {
            if (auto slice = sourceSliceFromLocation(source, alias->location))
            {
                std::string normalizedAlias = normalizeTypeAliasForLocalScope(*slice);
                typeAliases.push_back(normalizedAlias);

                if (auto aliasName = astNameToString(alias->name))
                    sharedTypeAliases.emplace_back(*aliasName, std::move(normalizedAlias));
            }
            continue;
        }

        if (auto* assign = stat->as<Luau::AstStatAssign>())
        {
            if (assign->vars.size == 1 && assign->values.size == 1)
            {
                if (auto* global = assign->vars.data[0]->as<Luau::AstExprGlobal>())
                {
                    if (auto valueSlice = sourceSliceFromLocation(source, assign->values.data[0]->location))
                        globalTypeBodies.emplace_back(global->name.value, "return " + *valueSlice);
                }

                if (auto oopGlobalName = oopGlobalNameFromCreateCall(assign->values.data[0]))
                {
                    if (auto varName = simpleExprName(assign->vars.data[0]))
                        addOopClassBinding(*oopGlobalName, *varName, extractOopPublicTypeFromAssertion(assign->values.data[0], source));
                }
            }
            continue;
        }

        if (auto* local = stat->as<Luau::AstStatLocal>())
        {
            size_t pairCount = std::min(local->vars.size, local->values.size);
            for (size_t i = 0; i < pairCount; ++i)
            {
                if (auto oopGlobalName = oopGlobalNameFromCreateCall(local->values.data[i]))
                {
                    if (auto varName = astNameToString(local->vars.data[i]->name))
                        addOopClassBinding(*oopGlobalName, *varName, extractOopPublicTypeFromAssertion(local->values.data[i], source));
                }
            }
            continue;
        }

        if (auto* fn = stat->as<Luau::AstStatFunction>())
        {
            if (auto* global = fn->name->as<Luau::AstExprGlobal>())
            {
                globalTypeBodies.emplace_back(global->name.value, "return (nil :: any) :: " + buildFunctionTypeFromAst(source, fn->func));
            }

            if (auto methodInfo = classPublicMethodName(fn->name))
            {
                const auto& [classLocalName, methodName] = *methodInfo;
                if (auto it = localToOopGlobal.find(classLocalName); it != localToOopGlobal.end())
                {
                    auto& methods = oopPublicMethodsByGlobal[it->second];
                    bool methodExists = std::any_of(methods.begin(), methods.end(),
                        [&](const auto& pair)
                        {
                            return pair.first == methodName;
                        });

                    if (!methodExists)
                        methods.emplace_back(methodName, buildFunctionTypeFromAst(source, fn->func));
                }
            }
        }
    }

    std::string prelude;
    prelude.reserve(512);
    prelude += "\n--!nolint\n";

    std::unordered_set<std::string> seenSharedTypeAliases;
    if (emitSharedTypeAliases)
    {
        for (const auto& [aliasName, aliasSource] : sharedTypeAliases)
        {
            if (!seenSharedTypeAliases.insert(aliasName).second)
                continue;

            prelude += aliasSource;
            prelude += "\n";
        }
    }

    std::unordered_set<std::string> seen;
    size_t index = 0;
    for (const auto& [name, typeBody] : globalTypeBodies)
    {
        if (!seen.insert(name).second)
            continue;

        prelude += "type __lsp_mta_global_" + providerTag + "_" + std::to_string(index++) + " = typeof((function()\n";
        for (const auto& alias : typeAliases)
        {
            prelude += alias;
            prelude += "\n";
        }
        prelude += typeBody;
        prelude += "\nend)())\n";
        prelude += "local ";
        prelude += name;
        prelude += " = (nil :: any) :: __lsp_mta_global_";
        prelude += providerTag + "_" + std::to_string(index - 1);
        prelude += "\n";
    }

    size_t oopIndex = 0;
    for (const auto& binding : oopClassBindings)
    {
        if (!seen.insert(binding.globalName).second)
            continue;

        const std::string publicTypeName = "__lsp_mta_oop_public_" + providerTag + "_" + std::to_string(oopIndex);
        const std::string classTypeName = "__lsp_mta_oop_class_" + providerTag + "_" + std::to_string(oopIndex);

        if (binding.explicitPublicType)
        {
            prelude += "type ";
            prelude += publicTypeName;
            prelude += " = typeof((function()\n";
            for (const auto& alias : typeAliases)
            {
                prelude += alias;
                prelude += "\n";
            }
            prelude += "return (nil :: any) :: ";
            prelude += *binding.explicitPublicType;
            prelude += "\nend)())\n";
        }
        else
        {
            auto methodsIt = oopPublicMethodsByGlobal.find(binding.globalName);
            if (methodsIt != oopPublicMethodsByGlobal.end())
            {
                for (size_t methodIndex = 0; methodIndex < methodsIt->second.size(); ++methodIndex)
                {
                    const auto& [methodName, functionType] = methodsIt->second[methodIndex];
                    const std::string methodTypeName =
                        "__lsp_mta_oop_method_" + providerTag + "_" + std::to_string(oopIndex) + "_" + std::to_string(methodIndex);

                    prelude += "type ";
                    prelude += methodTypeName;
                    prelude += " = ";
                    prelude += functionType;
                    prelude += "\n";
                }
            }

            prelude += "type ";
            prelude += publicTypeName;
            prelude += " = {\n";

            if (methodsIt != oopPublicMethodsByGlobal.end())
            {
                for (size_t methodIndex = 0; methodIndex < methodsIt->second.size(); ++methodIndex)
                {
                    const auto& [methodName, _] = methodsIt->second[methodIndex];
                    const std::string methodTypeName =
                        "__lsp_mta_oop_method_" + providerTag + "_" + std::to_string(oopIndex) + "_" + std::to_string(methodIndex);

                    prelude += "    ";
                    prelude += methodName;
                    prelude += ": ";
                    prelude += methodTypeName;
                    prelude += ",\n";
                }
            }

            prelude += "}\n";
        }

        prelude += "type ";
        prelude += classTypeName;
        prelude += " = ";
        prelude += publicTypeName;
        prelude += " & {\n";
        prelude += "    create: (self: ";
        prelude += publicTypeName;
        prelude += ", ...any) -> ";
        prelude += publicTypeName;
        prelude += ",\n";
        prelude += "    createInstance: (self: ";
        prelude += publicTypeName;
        prelude += ") -> ";
        prelude += publicTypeName;
        prelude += ",\n";
        prelude += "}\n";

        prelude += "local ";
        prelude += binding.globalName;
        prelude += " = (nil :: any) :: ";
        prelude += classTypeName;
        prelude += "\n";

        ++oopIndex;
    }

    // Fallback for files where AST extraction misses globals (e.g. partial parse states).
    // We still expose top-level global names for completion, even without rich type details.
    auto isIdentifierByte = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };

    size_t lineStart = 0;
    while (lineStart <= source.size())
    {
        size_t lineEnd = source.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = source.size();

        size_t i = lineStart;
        while (i < lineEnd && (source[i] == ' ' || source[i] == '\t' || source[i] == '\r'))
            ++i;

        if (i < lineEnd && isIdentifierByte(source[i]))
        {
            size_t j = i + 1;
            while (j < lineEnd && isIdentifierByte(source[j]))
                ++j;

            std::string name = source.substr(i, j - i);

            size_t k = j;
            while (k < lineEnd && (source[k] == ' ' || source[k] == '\t'))
                ++k;

            if (k < lineEnd && source[k] == '=' && seen.insert(name).second)
            {
                prelude += "local ";
                prelude += name;
                prelude += " = nil :: any\n";
            }
        }

        if (lineEnd == source.size())
            break;
        lineStart = lineEnd + 1;
    }

    if (seen.empty() && seenSharedTypeAliases.empty())
        return std::nullopt;

    return prelude;
}

static bool canSeeScriptType(MTAScriptType target, MTAScriptType provider)
{
    if (target == MTAScriptType::Client)
        return provider == MTAScriptType::Client || provider == MTAScriptType::Shared;
    if (target == MTAScriptType::Server)
        return provider == MTAScriptType::Server || provider == MTAScriptType::Shared;
    if (target == MTAScriptType::Shared)
        return provider == MTAScriptType::Shared;
    return false;
}
} // namespace

Luau::ModuleName WorkspaceFileResolver::getModuleName(const Uri& name) const
{
    // Handle non-file schemes
    if (name.scheme != "file")
        return name.toString();

    if (auto virtualPath = platform->resolveToVirtualPath(name))
        return *virtualPath;

    return name.fsPath();
}

Uri WorkspaceFileResolver::getUri(const Luau::ModuleName& moduleName) const
{
    if (platform->isVirtualPath(moduleName))
    {
        if (auto uri = platform->resolveToRealPath(moduleName))
            return *uri;
    }

    // TODO: right now we map to file paths for module names, unless it's a non-file uri. Should we store uris directly instead?
    // Then this would be Uri::parse
    return Uri::file(moduleName);
}

const TextDocument* WorkspaceFileResolver::getManagedTextDocument(const lsp::DocumentUri& uri) const
{
    auto it = managedFiles.find(uri);
    if (it != managedFiles.end())
        return &it->second;

    return nullptr;
}

const TextDocument* WorkspaceFileResolver::getTextDocument(const lsp::DocumentUri& uri) const
{
    // Get the original managed document
    auto* original = getManagedTextDocument(uri);
    if (!original)
        return nullptr;

    // Check cache for existing transformed document
    auto it = pluginDocuments.find(uri);
    if (it != pluginDocuments.end() && it->second && it->second->version() == original->version())
        return it->second.get();

    // Apply plugins to transform the document
    auto moduleName = getModuleName(uri);
    auto transformed = applyPluginTransformation(original->getText(), uri, moduleName);

    if (!transformed)
        return original;

    auto pluginDoc = std::make_unique<Luau::LanguageServer::Plugin::PluginTextDocument>(
        original->uri(),
        original->languageId(),
        original->version(),
        original->getText(),
        std::move(transformed->transformedSource),
        Luau::LanguageServer::Plugin::SourceMapping{std::move(transformed->edits)});

    pluginDocuments[uri] = std::move(pluginDoc);
    return pluginDocuments[uri].get();
}

void WorkspaceFileResolver::invalidatePluginDocument(const lsp::DocumentUri& uri)
{
    pluginDocuments.erase(uri);
}

void WorkspaceFileResolver::clearPluginDocuments()
{
    pluginDocuments.clear();
}

std::optional<Luau::LanguageServer::Plugin::TransformResult> WorkspaceFileResolver::applyPluginTransformation(
    const std::string& source, const Uri& uri, const std::string& moduleName) const
{
    std::vector<Luau::LanguageServer::Plugin::TextEdit> edits;

    if (pluginManager && pluginManager->hasPlugins())
    {
        // Plugins should not apply to their own source files
        if (!pluginManager->isPluginFile(uri))
        {
            auto pluginEdits = pluginManager->transform(source, uri, moduleName);
            edits.insert(edits.end(), pluginEdits.begin(), pluginEdits.end());
        }
    }

    if (auto mtaDeclarations = buildMtaGlobalDeclarations(uri))
    {
        auto pos = preludeInsertionPosition(source);
        edits.push_back(Luau::LanguageServer::Plugin::TextEdit{
            Luau::Location{pos, pos},
            *mtaDeclarations,
        });
    }

    if (edits.empty())
        return std::nullopt;

    try
    {
        return Luau::LanguageServer::Plugin::SourceMapping::fromEdits(source, edits);
    }
    catch (const std::exception& e)
    {
        if (client)
            client->sendLogMessage(lsp::MessageType::Error, "Failed to apply plugin transformation: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<std::string> WorkspaceFileResolver::buildMtaGlobalDeclarations(const Uri& uri) const
{
    auto metaUri = findResourceMetaFile(uri);
    if (!metaUri)
        return std::nullopt;

    auto metaSource = Luau::FileUtils::readFile(metaUri->fsPath());
    if (!metaSource)
        return std::nullopt;

    auto scriptEntries = parseMtaScriptEntries(*metaUri, *metaSource);
    if (scriptEntries.empty())
        return std::nullopt;

    const auto targetPath = normalizePath(uri.fsPath());

    MTAScriptType targetType = MTAScriptType::Unknown;
    for (const auto& entry : scriptEntries)
    {
        if (normalizePath(entry.uri.fsPath()) == targetPath)
        {
            targetType = entry.type;
            break;
        }
    }

    if (targetType == MTAScriptType::Unknown)
        return std::nullopt;

    auto readSourceForUri = [&](const Uri& sourceUri) -> std::optional<std::string>
    {
        std::string sourcePath = normalizePath(sourceUri.fsPath());

        if (auto* managed = getManagedTextDocument(sourceUri))
            return managed->getText();

        // Some clients may normalize file URIs differently (drive-letter casing, escaping).
        // Fall back to matching managed documents by normalized file path.
        for (const auto& [managedUri, managedDoc] : managedFiles)
        {
            if (normalizePath(managedUri.fsPath()) == sourcePath)
                return managedDoc.getText();
        }

        return Luau::FileUtils::readFile(sourceUri.fsPath());
    };

    std::unordered_set<std::string> seenFilePaths;
    std::string declarations;
    size_t providerOrdinal = 0;

    std::unordered_set<std::string> targetGlobalFunctions;
    if (auto targetSource = readSourceForUri(uri))
        targetGlobalFunctions = extractTopLevelGlobalFunctionNames(*targetSource);

    std::unordered_set<std::string> targetGlobalFunctionReferences;

    for (const auto& entry : scriptEntries)
    {
        if (!canSeeScriptType(targetType, entry.type))
            continue;

        std::string providerPath = normalizePath(entry.uri.fsPath());
        if (!seenFilePaths.insert(providerPath).second)
            continue;

        auto providerSource = readSourceForUri(entry.uri);

        if (!providerSource)
            continue;

        bool emitSharedTypeAliases = providerPath != targetPath;
        if (auto prelude = extractTypedGlobalsPrelude(*providerSource, std::to_string(providerOrdinal++), emitSharedTypeAliases))
            declarations += *prelude;

        if (!targetGlobalFunctions.empty() && providerPath != targetPath)
        {
            auto references = extractReferencedGlobalNames(*providerSource, targetGlobalFunctions);
            targetGlobalFunctionReferences.insert(references.begin(), references.end());
        }
    }

    if (auto usedGlobalsPrelude = buildUsedGlobalFunctionsPrelude(targetGlobalFunctions, targetGlobalFunctionReferences))
        declarations += *usedGlobalsPrelude;

    if (declarations.empty())
        return std::nullopt;

    return declarations;
}

bool WorkspaceFileResolver::isMtaGlobalFunctionUsedOutsideFile(const Uri& uri, const std::string& functionName) const
{
    if (!isIdentifier(functionName))
        return false;

    auto metaUri = findResourceMetaFile(uri);
    if (!metaUri)
        return false;

    auto metaSource = Luau::FileUtils::readFile(metaUri->fsPath());
    if (!metaSource)
        return false;

    auto scriptEntries = parseMtaScriptEntries(*metaUri, *metaSource);
    if (scriptEntries.empty())
        return false;

    const auto targetPath = normalizePath(uri.fsPath());
    MTAScriptType targetType = MTAScriptType::Unknown;
    for (const auto& entry : scriptEntries)
    {
        if (normalizePath(entry.uri.fsPath()) == targetPath)
        {
            targetType = entry.type;
            break;
        }
    }

    if (targetType == MTAScriptType::Unknown)
        return false;

    auto readSourceForUri = [&](const Uri& sourceUri) -> std::optional<std::string>
    {
        std::string sourcePath = normalizePath(sourceUri.fsPath());

        if (auto* managed = getManagedTextDocument(sourceUri))
            return managed->getText();

        for (const auto& [managedUri, managedDoc] : managedFiles)
        {
            if (normalizePath(managedUri.fsPath()) == sourcePath)
                return managedDoc.getText();
        }

        return Luau::FileUtils::readFile(sourceUri.fsPath());
    };

    std::unordered_set<std::string> tracked{functionName};
    std::unordered_set<std::string> seenFilePaths;
    for (const auto& entry : scriptEntries)
    {
        if (!canSeeScriptType(targetType, entry.type))
            continue;

        std::string providerPath = normalizePath(entry.uri.fsPath());
        if (providerPath == targetPath)
            continue;

        if (!seenFilePaths.insert(providerPath).second)
            continue;

        auto providerSource = readSourceForUri(entry.uri);
        if (!providerSource)
            continue;

        auto references = extractReferencedGlobalNames(*providerSource, tracked);
        if (references.count(functionName))
            return true;

        if (hasLikelyGlobalFunctionCall(*providerSource, functionName))
            return true;
    }

    return false;
}

bool WorkspaceFileResolver::isMtaGlobalFunctionDefinedOutsideFile(const Uri& uri, const std::string& functionName) const
{
    if (!isIdentifier(functionName))
        return false;

    auto metaUri = findResourceMetaFile(uri);
    if (!metaUri)
        return false;

    auto metaSource = Luau::FileUtils::readFile(metaUri->fsPath());
    if (!metaSource)
        return false;

    auto scriptEntries = parseMtaScriptEntries(*metaUri, *metaSource);
    if (scriptEntries.empty())
        return false;

    const auto targetPath = normalizePath(uri.fsPath());
    MTAScriptType targetType = MTAScriptType::Unknown;
    for (const auto& entry : scriptEntries)
    {
        if (normalizePath(entry.uri.fsPath()) == targetPath)
        {
            targetType = entry.type;
            break;
        }
    }

    if (targetType == MTAScriptType::Unknown)
        return false;

    auto readSourceForUri = [&](const Uri& sourceUri) -> std::optional<std::string>
    {
        std::string sourcePath = normalizePath(sourceUri.fsPath());

        if (auto* managed = getManagedTextDocument(sourceUri))
            return managed->getText();

        for (const auto& [managedUri, managedDoc] : managedFiles)
        {
            if (normalizePath(managedUri.fsPath()) == sourcePath)
                return managedDoc.getText();
        }

        return Luau::FileUtils::readFile(sourceUri.fsPath());
    };

    std::unordered_set<std::string> seenFilePaths;
    for (const auto& entry : scriptEntries)
    {
        if (!canSeeScriptType(targetType, entry.type))
            continue;

        std::string providerPath = normalizePath(entry.uri.fsPath());
        if (providerPath == targetPath)
            continue;

        if (!seenFilePaths.insert(providerPath).second)
            continue;

        auto providerSource = readSourceForUri(entry.uri);
        if (!providerSource)
            continue;

        auto globals = extractTopLevelGlobalFunctionNames(*providerSource);
        if (globals.count(functionName))
            return true;
    }

    return false;
}

const TextDocument* WorkspaceFileResolver::getTextDocumentFromModuleName(const Luau::ModuleName& name) const
{
    // managedFiles is keyed by URI. If module name is a URI that maps to a managed file, return that directly
    if (auto document = getTextDocument(Uri::parse(name)))
        return document;

    return getTextDocument(getUri(name));
}

TextDocumentPtr WorkspaceFileResolver::getOrCreateTextDocumentFromModuleName(const Luau::ModuleName& name)
{
    if (auto document = getTextDocumentFromModuleName(name))
        return TextDocumentPtr(document);

    if (auto filePath = platform->resolveToRealPath(name))
        if (auto source = readSource(name))
            return TextDocumentPtr(*filePath, "luau", source->source);

    return TextDocumentPtr(nullptr);
}

std::optional<Luau::SourceCode> WorkspaceFileResolver::readSource(const Luau::ModuleName& name)
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFileResolver::readSource", "LSP");
    auto uri = getUri(name);
    auto sourceType = platform->sourceCodeTypeFromPath(uri);

    // Check if this is a managed file - use getTextDocument which handles plugin transformation
    if (auto* textDoc = getTextDocument(uri))
        return Luau::SourceCode{textDoc->getText(), sourceType};

    // Fallback to reading from platform
    if (auto source = platform->readSourceCode(name, uri))
    {
        if (auto transformed = applyPluginTransformation(*source, uri, name))
            return Luau::SourceCode{std::move(transformed->transformedSource), sourceType};
        return Luau::SourceCode{*source, sourceType};
    }

    return std::nullopt;
}

std::optional<Luau::ModuleInfo> WorkspaceFileResolver::resolveModule(
    const Luau::ModuleInfo* context, Luau::AstExpr* node, const Luau::TypeCheckLimits& limits)
{
    return platform->resolveModule(context, node, limits);
}

std::string WorkspaceFileResolver::getHumanReadableModuleName(const Luau::ModuleName& name) const
{
    if (platform->isVirtualPath(name))
    {
        if (auto realPath = platform->resolveToRealPath(name))
        {
            return realPath->fsPath() + " [" + name + "]";
        }
        else
        {
            return name;
        }
    }
    else
    {
        return name;
    }
}

const Luau::Config& WorkspaceFileResolver::getConfig(const Luau::ModuleName& name, const Luau::TypeCheckLimits& limits) const
{
    LUAU_TIMETRACE_SCOPE("WorkspaceFileResolver::getConfig", "Frontend");
    auto uri = getUri(name);
    auto base = uri.parent();

    if (base && isInitLuauFile(uri))
        base = base->parent();

    if (!base)
        return defaultConfig;

    return readConfigRec(*base, limits);
}

std::optional<std::string> WorkspaceFileResolver::parseConfig(const Uri& configPath, const std::string& contents, Luau::Config& result, bool compat)
{
    LUAU_ASSERT(configPath.parent());

    Luau::ConfigOptions::AliasOptions aliasOpts;
    aliasOpts.configLocation = configPath.parent()->fsPath();
    aliasOpts.overwriteAliases = true;

    Luau::ConfigOptions opts;
    opts.aliasOptions = std::move(aliasOpts);
    opts.compat = compat;

    return Luau::parseConfig(contents, result, opts);
}

std::optional<std::string> WorkspaceFileResolver::parseLuauConfig(
    const Uri& configPath, const std::string& contents, Luau::Config& result, const Luau::TypeCheckLimits& limits)
{
    LUAU_ASSERT(configPath.parent());

    Luau::ConfigOptions::AliasOptions aliasOpts;
    aliasOpts.configLocation = configPath.parent()->fsPath();
    aliasOpts.overwriteAliases = true;

    Luau::InterruptCallbacks callbacks;
    LuauConfigInterruptInfo info{limits, configPath.fsPath()};
    callbacks.initCallback = [&info](lua_State* L)
    {
        lua_setthreaddata(L, &info);
    };
    callbacks.interruptCallback = [](lua_State* L, int gc)
    {
        auto* info = static_cast<LuauConfigInterruptInfo*>(lua_getthreaddata(L));
        if (info->limits.finishTime && Luau::TimeTrace::getClock() > *info->limits.finishTime)
            throw Luau::TimeLimitError{info->module};
        if (info->limits.cancellationToken && info->limits.cancellationToken->requested())
            throw Luau::UserCancelError{info->module};
    };

    return Luau::extractLuauConfig(contents, result, aliasOpts, std::move(callbacks));
}

const Luau::Config& WorkspaceFileResolver::readConfigRec(const Uri& uri, const Luau::TypeCheckLimits& limits) const
{
    auto it = configCache.find(uri);
    if (it != configCache.end())
        return it->second;

    Luau::Config result = defaultConfig;
    if (const auto& parent = uri.parent())
        result = readConfigRec(*parent, limits);

    auto configPath = uri.resolvePath(Luau::kConfigName);
    auto luauConfigPath = uri.resolvePath(Luau::kLuauConfigName);
    auto robloxRcPath = uri.resolvePath(".robloxrc");

    if (std::optional<std::string> contents = Luau::FileUtils::readFile(luauConfigPath.fsPath()))
    {
        if (client)
            client->sendLogMessage(lsp::MessageType::Info, "Loading Luau configuration from " + luauConfigPath.fsPath());

        std::optional<std::string> error = parseLuauConfig(configPath, *contents, result, limits);
        if (error)
        {
            if (client)
            {
                lsp::Diagnostic diagnostic{{{0, 0}, {0, 0}}};
                diagnostic.message = *error;
                diagnostic.severity = lsp::DiagnosticSeverity::Error;
                diagnostic.source = "Luau";
                client->publishDiagnostics({configPath, std::nullopt, {diagnostic}});
            }
            else
                // TODO: this should never be reached anymore
                std::cerr << configPath.toString() << ": " << *error;
        }
        else
        {
            if (client)
                // Clear errors presented for file
                client->publishDiagnostics({configPath, std::nullopt, {}});
        }
    }
    if (std::optional<std::string> contents = Luau::FileUtils::readFile(configPath.fsPath()))
    {
        if (client)
            client->sendLogMessage(lsp::MessageType::Info, "Loading Luau configuration from " + configPath.fsPath());

        std::optional<std::string> error = parseConfig(configPath, *contents, result);
        if (error)
        {
            if (client)
            {
                lsp::Diagnostic diagnostic{{{0, 0}, {0, 0}}};
                diagnostic.message = *error;
                diagnostic.severity = lsp::DiagnosticSeverity::Error;
                diagnostic.source = "Luau";
                client->publishDiagnostics({configPath, std::nullopt, {diagnostic}});
            }
            else
                // TODO: this should never be reached anymore
                std::cerr << configPath.toString() << ": " << *error;
        }
        else
        {
            if (client)
                // Clear errors presented for file
                client->publishDiagnostics({configPath, std::nullopt, {}});
        }
    }
    else if (std::optional<std::string> robloxRcContents = Luau::FileUtils::readFile(robloxRcPath.fsPath()))
    {
        if (client)
            client->sendLogMessage(lsp::MessageType::Info, "Loading Luau configuration from " + robloxRcPath.fsPath());

        // Backwards compatibility for .robloxrc files
        std::optional<std::string> error = parseConfig(robloxRcPath, *robloxRcContents, result, /* compat = */ true);
        if (error)
        {
            if (client)
            {
                lsp::Diagnostic diagnostic{{{0, 0}, {0, 0}}};
                diagnostic.message = *error;
                diagnostic.severity = lsp::DiagnosticSeverity::Error;
                diagnostic.source = "Luau";
                client->publishDiagnostics({robloxRcPath, std::nullopt, {diagnostic}});
            }
            else
                // TODO: this should never be reached anymore
                std::cerr << robloxRcPath.toString() << ": " << *error;
        }
        else
        {
            if (client)
                // Clear errors presented for file
                client->publishDiagnostics({robloxRcPath, std::nullopt, {}});
        }
    }

    return configCache[uri] = result;
}

void WorkspaceFileResolver::clearConfigCache()
{
    configCache.clear();
}

bool WorkspaceFileResolver::isPluginFile(const Luau::ModuleName& name) const
{
    if (!pluginManager || !pluginManager->hasPlugins())
        return false;

    auto uri = getUri(name);
    if (uri.scheme != "file")
        return false;

    return pluginManager->isPluginFile(uri);
}

std::optional<std::string> WorkspaceFileResolver::getEnvironmentForModule(const Luau::ModuleName& name) const
{
    if (isPluginFile(name))
        return "LSPPlugin";
    return std::nullopt;
}
