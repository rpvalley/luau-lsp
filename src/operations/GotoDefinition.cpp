#include "LSP/Workspace.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Parser.h"
#include "LSP/LuauExt.hpp"
#include "LuauFileUtils.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>

namespace
{
enum class MTAScriptType
{
    Unknown,
    Client,
    Server,
    Shared,
};

struct MtaScriptEntry
{
    Uri uri;
    MTAScriptType type = MTAScriptType::Unknown;
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

static std::optional<Luau::Location> findTopLevelGlobalDefinition(const std::string& source, const std::string& globalName)
{
    std::string parseSource = source;
    size_t parseIndex = 0;
    while ((parseIndex = parseSource.find("export type", parseIndex)) != std::string::npos)
    {
        parseSource.replace(parseIndex, 7, "       ");
        parseIndex += 11;
    }

    Luau::Allocator allocator;
    Luau::AstNameTable names{allocator};
    Luau::ParseOptions options;
    Luau::ParseResult parseResult = Luau::Parser::parse(parseSource.c_str(), parseSource.size(), names, allocator, options);
    if (!parseResult.root)
        return std::nullopt;

    for (Luau::AstStat* stat : parseResult.root->body)
    {
        if (auto* assign = stat->as<Luau::AstStatAssign>())
        {
            for (size_t i = 0; i < assign->vars.size; ++i)
            {
                if (auto* global = assign->vars.data[i]->as<Luau::AstExprGlobal>())
                {
                    if (global->name.value == globalName)
                        return global->location;
                }
            }
            continue;
        }

        if (auto* fn = stat->as<Luau::AstStatFunction>())
        {
            if (auto* global = fn->name->as<Luau::AstExprGlobal>())
            {
                if (global->name.value == globalName)
                    return global->location;
            }
        }
    }

    return std::nullopt;
}

static bool isIdentifierByte(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static std::optional<std::string> getIdentifierAtPosition(const TextDocument* textDocument, const Luau::Position& position)
{
    if (!textDocument)
        return std::nullopt;

    if (position.line >= textDocument->lineCount())
        return std::nullopt;

    std::string line = textDocument->getLine(position.line);
    if (line.empty())
        return std::nullopt;

    size_t cursor = std::min<size_t>(position.column, line.size());

    if (cursor > 0 && !isIdentifierByte(line[cursor]) && isIdentifierByte(line[cursor - 1]))
        cursor -= 1;

    if (cursor >= line.size() || !isIdentifierByte(line[cursor]))
        return std::nullopt;

    size_t start = cursor;
    while (start > 0 && isIdentifierByte(line[start - 1]))
        --start;

    size_t end = cursor;
    while (end + 1 < line.size() && isIdentifierByte(line[end + 1]))
        ++end;

    return line.substr(start, end - start + 1);
}
} // namespace

struct LocationInformation
{
    std::optional<std::string> definitionModuleName;
    std::optional<Luau::Location> location;
    Luau::TypeId ty;
};

static std::optional<LocationInformation> findLocationForSymbol(
    const Luau::ModulePtr& module, const Luau::Position& position, const Luau::Symbol& symbol)
{
    auto scope = Luau::findScopeAtPosition(*module, position);
    auto ty = scope->lookup(symbol);
    if (!ty)
        return std::nullopt;
    ty = Luau::follow(*ty);
    return LocationInformation{Luau::getDefinitionModuleName(*ty), getLocation(*ty), *ty};
}

static std::vector<LocationInformation> findLocationsForIndex(const Luau::ModulePtr& module, const Luau::AstExpr* base, const Luau::Name& name)
{
    auto baseTy = module->astTypes.find(base);
    if (!baseTy)
        return {};
    auto baseTyFollowed = Luau::follow(*baseTy);

    std::vector<LocationInformation> results;
    for (const auto& [realBaseTy, prop] : lookupProp(baseTyFollowed, name))
    {
        auto location = prop.location ? prop.location : prop.typeLocation;
        if (!prop.readTy)
            continue;

        // Deduplicate by location to avoid returning multiple identical results
        // (e.g., when a union has multiple instantiations of the same generic type)
        bool isDuplicate = std::any_of(results.begin(), results.end(),
            [&](const LocationInformation& existing)
            {
                return existing.location == location;
            });
        if (!isDuplicate)
            results.push_back(LocationInformation{Luau::getDefinitionModuleName(realBaseTy), location, *prop.readTy});
    }

    return results;
}

static std::vector<LocationInformation> findLocationsForExpr(const Luau::ModulePtr& module, const Luau::AstExpr* expr, const Luau::Position& position)
{
    if (auto local = expr->as<Luau::AstExprLocal>())
    {
        if (auto loc = findLocationForSymbol(module, position, local->local))
            return {*loc};
    }
    else if (auto global = expr->as<Luau::AstExprGlobal>())
    {
        if (auto loc = findLocationForSymbol(module, position, global->name))
            return {*loc};
    }
    else if (auto indexname = expr->as<Luau::AstExprIndexName>())
        return findLocationsForIndex(module, indexname->expr, indexname->index.value);
    else if (auto indexexpr = expr->as<Luau::AstExprIndexExpr>())
    {
        if (auto string = indexexpr->index->as<Luau::AstExprConstantString>())
            return findLocationsForIndex(module, indexexpr->expr, std::string(string->value.data, string->value.size));
    }

    return {};
}

lsp::DefinitionResult WorkspaceFolder::gotoDefinition(const lsp::DefinitionParams& params, const LSPCancellationToken& cancellationToken)
{
    lsp::DefinitionResult result{};

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());
    auto position = textDocument->convertPosition(params.position);

    // Run the type checker to ensure we are up to date
    checkStrict(moduleName, cancellationToken);
    throwIfCancelled(cancellationToken);

    auto sourceModule = frontend.getSourceModule(moduleName);
    // TODO: fix "forAutocomplete"
    auto module = getModule(moduleName, /* forAutocomplete: */ true);
    if (!sourceModule || !module)
        return result;

    auto binding = Luau::findBindingAtPosition(*module, *sourceModule, position);
    if (binding)
    {
        bool isGlobalLookup = false;
        if (auto nodeForBinding = findNodeOrTypeAtPosition(*sourceModule, position))
        {
            if (auto expr = nodeForBinding->asExpr())
            {
                if (expr->as<Luau::AstExprGlobal>())
                    isGlobalLookup = true;
                else if (auto* indexName = expr->as<Luau::AstExprIndexName>())
                    isGlobalLookup = indexName->expr->as<Luau::AstExprGlobal>() != nullptr;
                else if (auto* indexExpr = expr->as<Luau::AstExprIndexExpr>())
                    isGlobalLookup = indexExpr->expr->as<Luau::AstExprGlobal>() != nullptr;
            }
        }

        // If it points to a global definition (i.e. at pos 0,0), return nothing
        // and fall through to MTA meta-based lookup below.
        if (!(binding->location.begin == Luau::Position{0, 0} && binding->location.end == Luau::Position{0, 0}) && !isGlobalLookup)
        {
            // Follow through the binding reference if it is a function type
            // This is particularly useful for `local X = require(...)` where `X` is a function - we want the actual function definition
            // TODO: Can we get further references for other types?
            auto ftv = Luau::get<Luau::FunctionType>(Luau::follow(binding->typeId));
            if (ftv && ftv->definition && ftv->definition->definitionModuleName)
            {
                if (auto document = fileResolver.getOrCreateTextDocumentFromModuleName(ftv->definition->definitionModuleName.value()))
                {
                    result.emplace_back(lsp::Location{document->uri(), lsp::Range{document->convertPosition(ftv->definition->originalNameLocation.begin),
                                                                           document->convertPosition(ftv->definition->originalNameLocation.end)}});
                    return result;
                }
            }

            result.emplace_back(lsp::Location{params.textDocument.uri,
                lsp::Range{textDocument->convertPosition(binding->location.begin), textDocument->convertPosition(binding->location.end)}});
            return result;
        }
    }

    auto node = findNodeOrTypeAtPosition(*sourceModule, position);
    if (node)
    {
        if (auto expr = node->asExpr())
        {
            for (const auto& [definitionModuleName, location, _] : findLocationsForExpr(module, expr, position))
            {
                if (location)
                {
                    if (definitionModuleName)
                    {
                        if (auto uri = platform->resolveToRealPath(*definitionModuleName))
                        {
                            if (auto document = fileResolver.getOrCreateTextDocumentFromModuleName(*definitionModuleName))
                            {
                                result.emplace_back(lsp::Location{
                                    *uri, lsp::Range{document->convertPosition(location->begin), document->convertPosition(location->end)}});
                            }
                        }
                    }
                    else
                    {
                        result.emplace_back(lsp::Location{params.textDocument.uri,
                            lsp::Range{textDocument->convertPosition(location->begin), textDocument->convertPosition(location->end)}});
                    }
                }
            }
        }
        else if (auto reference = node->as<Luau::AstTypeReference>())
        {
            auto uri = params.textDocument.uri;
            TextDocumentPtr referenceTextDocument(textDocument);
            std::optional<Luau::Location> location = std::nullopt;

            auto scope = Luau::findScopeAtPosition(*module, position);
            if (!scope)
                return result;

            if (reference->prefix)
            {
                if (auto importedName = lookupImportedModule(*scope, reference->prefix.value().value))
                {
                    auto importedModule = getModule(*importedName, /* forAutocomplete: */ true);
                    if (!importedModule)
                        return result;

                    const auto it = importedModule->exportedTypeBindings.find(reference->name.value);
                    if (it == importedModule->exportedTypeBindings.end() || !it->second.definitionLocation)
                        return result;

                    referenceTextDocument = fileResolver.getOrCreateTextDocumentFromModuleName(*importedName);
                    location = *it->second.definitionLocation;
                }
                else
                    return result;
            }
            else
            {
                location = lookupTypeLocation(*scope, reference->name.value);
            }

            if (!referenceTextDocument || !location)
                return result;

            result.emplace_back(lsp::Location{referenceTextDocument->uri(),
                lsp::Range{referenceTextDocument->convertPosition(location->begin), referenceTextDocument->convertPosition(location->end)}});
        }
    }

    // Fallback: if no results found so far, we can try checking if this is within a require statement
    if (result.empty())
    {
        std::optional<std::string> globalName;

        if (node)
        {
            if (auto expr = node->asExpr())
            {
                if (auto* global = expr->as<Luau::AstExprGlobal>())
                    globalName = global->name.value;
                else if (auto* indexName = expr->as<Luau::AstExprIndexName>())
                {
                    if (auto* baseGlobal = indexName->expr->as<Luau::AstExprGlobal>())
                        globalName = baseGlobal->name.value;
                }
                else if (auto* indexExpr = expr->as<Luau::AstExprIndexExpr>())
                {
                    if (auto* baseGlobal = indexExpr->expr->as<Luau::AstExprGlobal>())
                        globalName = baseGlobal->name.value;
                }
            }
        }

        if (!globalName)
            globalName = getIdentifierAtPosition(textDocument, position);

        if (globalName)
        {
            auto metaUri = findResourceMetaFile(params.textDocument.uri);
            if (metaUri)
            {
                auto metaSource = Luau::FileUtils::readFile(metaUri->fsPath());
                if (metaSource)
                {
                    auto entries = parseMtaScriptEntries(*metaUri, *metaSource);
                    if (!entries.empty())
                    {
                        const std::string targetPath = normalizePath(params.textDocument.uri.fsPath());
                        MTAScriptType targetType = MTAScriptType::Unknown;

                        for (const auto& entry : entries)
                        {
                            if (normalizePath(entry.uri.fsPath()) == targetPath)
                            {
                                targetType = entry.type;
                                break;
                            }
                        }

                        std::unordered_set<std::string> seenFilePaths;
                        for (const auto& entry : entries)
                        {
                            if (targetType != MTAScriptType::Unknown && !canSeeScriptType(targetType, entry.type))
                                continue;

                            std::string providerPath = normalizePath(entry.uri.fsPath());
                            if (!seenFilePaths.insert(providerPath).second)
                                continue;

                            std::optional<std::string> providerSource = Luau::FileUtils::readFile(entry.uri.fsPath());
                            if (!providerSource)
                                continue;

                            auto globalLocation = findTopLevelGlobalDefinition(*providerSource, *globalName);
                            if (!globalLocation)
                                continue;

                            if (const TextDocument* providerTextDoc = fileResolver.getTextDocument(entry.uri))
                            {
                                result.emplace_back(lsp::Location{providerTextDoc->uri(),
                                    lsp::Range{providerTextDoc->convertPosition(globalLocation->begin),
                                        providerTextDoc->convertPosition(globalLocation->end)}});
                                break;
                            }

                            TextDocument tempProviderDoc(entry.uri, "luau", 0, *providerSource);
                            result.emplace_back(lsp::Location{entry.uri,
                                lsp::Range{tempProviderDoc.convertPosition(globalLocation->begin),
                                    tempProviderDoc.convertPosition(globalLocation->end)}});
                            break;
                        }
                }
            }
        }
        }

        if (!result.empty())
            return result;

        auto ancestry = Luau::findAstAncestryOfPosition(*sourceModule, position);
        if (ancestry.size() >= 2)
        {
            if (auto call = ancestry[ancestry.size() - 2]->as<Luau::AstExprCall>(); call && types::matchRequire(*call))
            {
                if (auto moduleInfo = frontend.moduleResolver.resolveModuleInfo(moduleName, *call))
                {
                    if (auto uri = platform->resolveToRealPath(moduleInfo->name))
                    {
                        result.emplace_back(lsp::Location{*uri, lsp::Range{{0, 0}, {0, 0}}});
                    }
                }
            }
        }
    }

    // Remove duplicate elements within the result
    // TODO: This is O(n^2). It shouldn't matter too much, since right now there will only be at most 2 elements.
    // But maybe we can remove the need for this in the first place?
    auto end = result.end();
    for (auto it = result.begin(); it != end; ++it)
        end = std::remove(it + 1, end, *it);

    result.erase(end, result.end());

    return result;
}

std::optional<lsp::Location> WorkspaceFolder::gotoTypeDefinition(
    const lsp::TypeDefinitionParams& params, const LSPCancellationToken& cancellationToken)
{
    // If its a binding, we should find its assigned type if possible, and then find the definition of that type
    // If its a type, then just find the definintion of that type (i.e. the type alias)

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());
    auto position = textDocument->convertPosition(params.position);

    // Run the type checker to ensure we are up to date
    checkStrict(moduleName, cancellationToken);
    throwIfCancelled(cancellationToken);

    auto sourceModule = frontend.getSourceModule(moduleName);
    // TODO: fix "forAutocomplete"
    auto module = getModule(moduleName, /* forAutocomplete: */ true);
    if (!sourceModule || !module)
        return std::nullopt;

    auto node = findNodeOrTypeAtPosition(*sourceModule, position);
    if (!node)
        return std::nullopt;

    auto findTypeLocation = [this, textDocument, &module, &position](Luau::AstType* type) -> std::optional<lsp::Location>
    {
        // TODO: should we only handle references here? what if its an actual type
        if (auto reference = type->as<Luau::AstTypeReference>())
        {
            TextDocumentPtr referenceTextDocument(textDocument);
            std::optional<Luau::Location> location = std::nullopt;

            auto scope = Luau::findScopeAtPosition(*module, position);
            if (!scope)
                return std::nullopt;

            if (reference->prefix)
            {
                if (auto importedName = lookupImportedModule(*scope, reference->prefix.value().value))
                {
                    auto importedModule = getModule(*importedName, /* forAutocomplete: */ true);
                    if (!importedModule)
                        return std::nullopt;

                    const auto it = importedModule->exportedTypeBindings.find(reference->name.value);
                    if (it == importedModule->exportedTypeBindings.end() || !it->second.definitionLocation)
                        return std::nullopt;

                    referenceTextDocument = fileResolver.getOrCreateTextDocumentFromModuleName(*importedName);
                    location = *it->second.definitionLocation;
                }
                else
                    return std::nullopt;
            }
            else
            {
                location = lookupTypeLocation(*scope, reference->name.value);
            }

            if (!referenceTextDocument || !location)
                return std::nullopt;

            return lsp::Location{referenceTextDocument->uri(),
                lsp::Range{referenceTextDocument->convertPosition(location->begin), referenceTextDocument->convertPosition(location->end)}};
        }
        return std::nullopt;
    };

    if (auto type = node->asType())
    {
        return findTypeLocation(type);
    }
    else if (auto typeAlias = node->as<Luau::AstStatTypeAlias>())
    {
        return findTypeLocation(typeAlias->type);
    }
    else if (auto expr = node->asExpr())
    {
        if (auto ty = module->astTypes.find(expr))
        {
            auto followedTy = Luau::follow(*ty);
            auto definitionModuleName = Luau::getDefinitionModuleName(followedTy);
            auto location = getLocation(followedTy);

            if (definitionModuleName && location)
            {
                auto document = fileResolver.getOrCreateTextDocumentFromModuleName(*definitionModuleName);
                if (document)
                    return lsp::Location{
                        document->uri(), lsp::Range{document->convertPosition(location->begin), document->convertPosition(location->end)}};
            }
        }
    }

    return std::nullopt;
}
