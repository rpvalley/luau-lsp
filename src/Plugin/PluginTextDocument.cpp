#include "Plugin/PluginTextDocument.hpp"

namespace Luau::LanguageServer::Plugin
{

PluginTextDocument::PluginTextDocument(
    lsp::DocumentUri uri,
    std::string languageId,
    size_t version,
    std::string originalContent,
    std::string transformedContent,
    SourceMapping mapping)
    : TextDocument(uri, languageId, version, std::move(transformedContent))
    , originalDoc(std::move(uri), std::move(languageId), version, std::move(originalContent))
    , mapping(std::move(mapping))
{
}

Luau::Position PluginTextDocument::convertPosition(const lsp::Position& position) const
{
    // Convert LSP position (UTF-16) to Luau position using ORIGINAL content
    Luau::Position originalPos = originalDoc.convertPosition(position);

    // Then map from original to transformed position
    Luau::Position transformedPos = originalPos;
    if (auto mapped = mapping.originalToTransformed(originalPos))
        transformedPos = *mapped;

    // Clamp to valid transformed bounds.
    size_t transformedLineCount = lineCount();
    if (transformedLineCount == 0)
        return Luau::Position{0, 0};

    if (transformedPos.line >= transformedLineCount)
        transformedPos.line = static_cast<unsigned int>(transformedLineCount - 1);

    const std::string transformedLineText = getLine(transformedPos.line);
    if (transformedPos.column > transformedLineText.size())
        transformedPos.column = static_cast<unsigned int>(transformedLineText.size());

    return transformedPos;
}

lsp::Position PluginTextDocument::convertPosition(const Luau::Position& position) const
{
    // Map from transformed to original position
    Luau::Position originalPos = position;
    if (auto mapped = mapping.transformedToOriginal(position))
        originalPos = *mapped;

    // Mapping can land outside original bounds for synthesized-only regions.
    // Clamp to a valid original position before converting to LSP coordinates.
    size_t originalLineCount = originalDoc.lineCount();
    if (originalLineCount == 0)
        return lsp::Position{0, 0};

    if (originalPos.line >= originalLineCount)
        originalPos.line = static_cast<unsigned int>(originalLineCount - 1);

    const std::string lineText = originalDoc.getLine(originalPos.line);
    if (originalPos.column > lineText.size())
        originalPos.column = static_cast<unsigned int>(lineText.size());

    // Convert original Luau position to LSP position using ORIGINAL content
    return originalDoc.convertPosition(originalPos);
}

std::string PluginTextDocument::getOriginalText() const
{
    return originalDoc.getText();
}

} // namespace Luau::LanguageServer::Plugin
