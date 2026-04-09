#include "doctest.h"
#include "Fixture.h"
#include "RobloxTestConstants.h"
#include "Platform/RobloxPlatform.hpp"
#include "LSP/IostreamHelpers.hpp"
#include "LSP/Completion.hpp"
#include "Platform/InstanceRequireAutoImporter.hpp"
#include "TempDir.h"

std::optional<lsp::CompletionItem> getItem(const std::vector<lsp::CompletionItem>& items, const std::string& label)
{
    for (const auto& item : items)
        if (item.label == label)
            return item;
    return std::nullopt;
}

lsp::CompletionItem requireItem(const std::vector<lsp::CompletionItem>& items, const std::string& label)
{
    auto item = getItem(items, label);
    REQUIRE_MESSAGE(item.has_value(), "no item found ", label);
    return item.value();
}

struct FragmentAutocompleteFixture : Fixture
{
    FragmentAutocompleteFixture()
    {
        client->globalConfig.completion.enableFragmentAutocomplete = true;
    }

    std::vector<lsp::CompletionItem> fragmentAutocomplete(const std::string& oldSource, const std::string& newSource, const lsp::Position& position)
    {
        // Enable pull-based diagnostics, otherwise updateTextDocument will trigger a diagnostic check
        client->capabilities.textDocument = lsp::TextDocumentClientCapabilities{};
        client->capabilities.textDocument->diagnostic = lsp::DiagnosticClientCapabilities{};

        auto uri = newDocument("foo.luau", oldSource);
        auto moduleName = workspace.fileResolver.getModuleName(uri);
        bool forAutocomplete = !FFlag::LuauSolverV2;

        // Perform an initial type check
        workspace.checkStrict(moduleName, /* cancellationToken= */ nullptr, /* forAutocomplete= */ forAutocomplete);

        // Update the text document
        updateDocument(uri, newSource);

        REQUIRE(workspace.frontend.allModuleDependenciesValid(moduleName, forAutocomplete));
        REQUIRE(workspace.frontend.isDirty(moduleName, forAutocomplete));

        lsp::CompletionParams params;
        params.textDocument = lsp::TextDocumentIdentifier{uri};
        params.position = position;

        auto results = workspace.completion(params, /* cancellationToken= */ nullptr);

        // Module should still be dirty afterwards if fragment autocomplete worked
        REQUIRE(workspace.frontend.isDirty(moduleName, forAutocomplete));

        return results;
    }
};

TEST_SUITE_BEGIN("Autocomplete");

TEST_CASE_FIXTURE(Fixture, "mta_meta_shared_globals_are_visible_to_client_completion")
{
    switchToStandardPlatform();

    tempDir.write_child("meta.xml", R"(
        <meta>
            <script src="shared.luau" type="shared" />
            <script src="client.luau" type="client" />
            <script src="server.luau" type="server" />
        </meta>
    )");

    const std::string sharedSource = R"(
        type NameTypes = {
            [string]: string,
        }

        NAMES = {
            class = "class",
            namespace = "namespace",
        } :: NameTypes
    )";
    tempDir.write_child("shared.luau", sharedSource);
    newDocument("shared.luau", sharedSource);

    auto [source, marker] = sourceWithMarker("NA|");
    tempDir.write_child("client.luau", source);
    auto uri = newDocument("client.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    CHECK(getItem(result, "NAMES").has_value());

}

TEST_CASE_FIXTURE(Fixture, "mta_meta_export_type_global_is_visible_in_client_completion")
{
    switchToStandardPlatform();

    tempDir.write_child("meta.xml", R"(
        <meta>
            <lua client="luau" server="luau" both="luau" />
            <script src="shared.luau" type="server" />
            <script src="client.luau" type="server" />
        </meta>
    )");

    const std::string sharedSource = R"(
        export type NamesType = {
            name: string,
            id: number,
        }

        SOME_VARIABLE = {
            name = "John Doe",
            id = 12345,
        } :: NamesType
    )";

    tempDir.write_child("shared.luau", sharedSource);
    newDocument("shared.luau", sharedSource);

    auto [clientSource, marker] = sourceWithMarker("SO|");
    tempDir.write_child("client.luau", clientSource);
    auto clientUri = newDocument("client.luau", clientSource);

    auto transformedClientDoc = workspace.fileResolver.getTextDocument(clientUri);
    REQUIRE(transformedClientDoc != nullptr);
    CHECK_NE(transformedClientDoc->getText().find("SOME_VARIABLE = (nil :: any) :: __lsp_mta_global_"), std::string::npos);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{clientUri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    CHECK(getItem(result, "SOME_VARIABLE").has_value());

    auto [memberSource, memberMarker] = sourceWithMarker("SOME_VARIABLE.|");
    tempDir.write_child("client.luau", memberSource);
    updateDocument(clientUri, memberSource);

    lsp::CompletionParams memberParams;
    memberParams.textDocument = lsp::TextDocumentIdentifier{clientUri};
    memberParams.position = memberMarker;

    auto memberResult = workspace.completion(memberParams, nullptr);
    CHECK(getItem(memberResult, "name").has_value());
    CHECK(getItem(memberResult, "id").has_value());
}

TEST_CASE_FIXTURE(Fixture, "mta_meta_client_globals_are_not_visible_to_server_completion")
{
    switchToStandardPlatform();

    tempDir.write_child("meta.xml", R"(
        <meta>
            <script src="shared.luau" type="shared" />
            <script src="client_globals.luau" type="client" />
            <script src="client.luau" type="client" />
            <script src="server.luau" type="server" />
        </meta>
    )");

    newDocument("shared.luau", "SHARED_VALUE = 1");
    newDocument("client_globals.luau", "CLIENT_ONLY = 123");

    auto [clientSource, clientMarker] = sourceWithMarker("CL|");
    auto clientUri = newDocument("client.luau", clientSource);

    lsp::CompletionParams clientParams;
    clientParams.textDocument = lsp::TextDocumentIdentifier{clientUri};
    clientParams.position = clientMarker;

    auto clientResult = workspace.completion(clientParams, nullptr);
    CHECK(getItem(clientResult, "CLIENT_ONLY").has_value());

    auto [serverSource, serverMarker] = sourceWithMarker("CL|");
    auto serverUri = newDocument("server.luau", serverSource);

    lsp::CompletionParams serverParams;
    serverParams.textDocument = lsp::TextDocumentIdentifier{serverUri};
    serverParams.position = serverMarker;

    auto serverResult = workspace.completion(serverParams, nullptr);
    CHECK_FALSE(getItem(serverResult, "CLIENT_ONLY").has_value());
}

TEST_CASE("merged_workspace_exports_completion_is_resource_scoped")
{
    TempDir tempDir("luau_lsp_workspace_exports_completion");

    tempDir.write_child("types/inventory.d.lua", R"(
        declare exports: {
            ['valley_inventory']: {
                funcao: (self: any, s: string) -> number
            }
        }
    )");
    tempDir.write_child("types/test_b.d.lua", R"(
        declare exports: {
            ['valley_test_b']: {
                makeApi: (self: any, s: string) -> number
            }
        }
    )");

    TestClient client;
    auto workspace = WorkspaceFolder(&client, "$TEST_WORKSPACE", Uri::file(tempDir.path()), std::nullopt);

    auto config = Luau::LanguageServer::defaultTestClientConfiguration();
    config.platform.type = LSPPlatformConfig::Standard;

    workspace.setupWithConfiguration(config);
    workspace.isReady = true;

    auto [topLevelSource, topLevelMarker] = sourceWithMarker("exp|");
    auto uri = Luau::LanguageServer::newDocument(workspace, "main.luau", topLevelSource);

    auto updateSource = [&](const std::string& source)
    {
        lsp::DidChangeTextDocumentParams params;
        params.textDocument = {{uri}, 0};
        params.contentChanges = {{std::nullopt, source}};
        workspace.updateTextDocument(uri, params);
    };

    lsp::CompletionParams topLevelParams;
    topLevelParams.textDocument = lsp::TextDocumentIdentifier{uri};
    topLevelParams.position = topLevelMarker;

    auto topLevelResult = workspace.completion(topLevelParams, nullptr);
    CHECK(getItem(topLevelResult, "exports").has_value());

    auto [inventorySource, inventoryMarker] = sourceWithMarker("exports['valley_inventory']:|");
    updateSource(inventorySource);

    lsp::CompletionParams inventoryParams;
    inventoryParams.textDocument = lsp::TextDocumentIdentifier{uri};
    inventoryParams.position = inventoryMarker;

    auto inventoryResult = workspace.completion(inventoryParams, nullptr);
    CHECK(getItem(inventoryResult, "funcao").has_value());
    CHECK_FALSE(getItem(inventoryResult, "makeApi").has_value());

    auto [testBSource, testBMarker] = sourceWithMarker("exports['valley_test_b']:|");
    updateSource(testBSource);

    lsp::CompletionParams testBParams;
    testBParams.textDocument = lsp::TextDocumentIdentifier{uri};
    testBParams.position = testBMarker;

    auto testBResult = workspace.completion(testBParams, nullptr);
    CHECK(getItem(testBResult, "makeApi").has_value());
    CHECK_FALSE(getItem(testBResult, "funcao").has_value());
}

TEST_CASE_FIXTURE(Fixture, "metatable_oop_completions_for_class_public_and_self")
{
    switchToStandardPlatform();

    auto [classSource, classMarker] = sourceWithMarker(R"(
        local oop = {}
        function oop:create(...)
            return {}
        end

        local app = oop:create("ola", nil, nil, true)

        function app.public:getBark()
            return "woof"
        end

        function app.public:load()
            self:getBark()
            self.health = 100
        end

        app.|
    )");

    auto uri = newDocument("metatable_oop.luau", classSource);

    lsp::CompletionParams classParams;
    classParams.textDocument = lsp::TextDocumentIdentifier{uri};
    classParams.position = classMarker;

    auto classResult = workspace.completion(classParams, nullptr);
    CHECK(getItem(classResult, "public").has_value());
    CHECK(getItem(classResult, "private").has_value());
    CHECK(getItem(classResult, "protected").has_value());

    auto [publicSource, publicMarker] = sourceWithMarker(R"(
        local oop = {}
        function oop:create(...)
            return {}
        end

        local app = oop:create("ola", nil, nil, true)

        function app.public:getBark()
            return "woof"
        end

        function app.public:load()
            self:getBark()
            self.health = 100
        end

        app.public:|
    )");

    updateDocument(uri, publicSource);

    lsp::CompletionParams publicParams;
    publicParams.textDocument = lsp::TextDocumentIdentifier{uri};
    publicParams.position = publicMarker;

    auto publicResult = workspace.completion(publicParams, nullptr);
    CHECK(getItem(publicResult, "getBark").has_value());
    CHECK(getItem(publicResult, "load").has_value());

    auto [selfSource, selfMarker] = sourceWithMarker(R"(
        local oop = {}
        function oop:create(...)
            return {}
        end

        local app = oop:create("ola", nil, nil, true)

        function app.public:getBark()
            return "woof"
        end

        function app.public:load()
            self.|
        end
    )");

    updateDocument(uri, selfSource);

    lsp::CompletionParams selfParams;
    selfParams.textDocument = lsp::TextDocumentIdentifier{uri};
    selfParams.position = selfMarker;

    auto selfResult = workspace.completion(selfParams, nullptr);
    CHECK(getItem(selfResult, "getBark").has_value());
    CHECK(getItem(selfResult, "load").has_value());
}

TEST_CASE_FIXTURE(Fixture, "function_autocomplete_has_documentation")
{
    auto [source, marker] = sourceWithMarker(R"(
        --- This is a function documentation comment
        local function foo()
        end

        local x = |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = requireItem(result, "foo");

    REQUIRE(item.documentation);
    CHECK_EQ(item.documentation->kind, lsp::MarkupKind::Markdown);
    trim(item.documentation->value);
    CHECK_EQ(item.documentation->value, "This is a function documentation comment");
}

TEST_CASE_FIXTURE(Fixture, "table_property_autocomplete_has_documentation")
{
    auto [source, marker] = sourceWithMarker(R"(
        local tbl = {
            --- This is a property on the table!
            prop = true,
        }

        local x = tbl.|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = requireItem(result, "prop");

    REQUIRE(item.documentation);
    CHECK_EQ(item.documentation->kind, lsp::MarkupKind::Markdown);
    trim(item.documentation->value);
    CHECK_EQ(item.documentation->value, "This is a property on the table!");
}

TEST_CASE_FIXTURE(Fixture, "show_documentation_when_autocompleting_property_in_table")
{
    auto [source, marker] = sourceWithMarker(R"(
        type Tbl = {
            --- Some documentation
            Property: number
        }

        local var: Tbl = {
            Prop|
        }
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    auto propertyEntry = getItem(result, "Property");
    REQUIRE(propertyEntry);
    CHECK_EQ(propertyEntry->kind, lsp::CompletionItemKind::Field);
    REQUIRE(propertyEntry->documentation);
    CHECK_EQ(propertyEntry->documentation->kind, lsp::MarkupKind::Markdown);
    trim(propertyEntry->documentation->value);
    CHECK_EQ(propertyEntry->documentation->value, "Some documentation");
}

TEST_CASE_FIXTURE(FragmentAutocompleteFixture, "fragment_autocomplete_table_property_autocomplete_has_documentation")
{
    auto oldSource = R"(
        local tbl = {
            --- This is a property on the table!
            prop = true,
        }

        local x = tbl
    )";

    auto [source, marker] = sourceWithMarker(R"(
        local tbl = {
            --- This is a property on the table!
            prop = true,
        }

        local x = tbl.|
    )");

    auto result = fragmentAutocomplete(oldSource, source, marker);
    auto item = requireItem(result, "prop");

    REQUIRE(item.documentation);
    CHECK_EQ(item.documentation->kind, lsp::MarkupKind::Markdown);
    trim(item.documentation->value);
    CHECK_EQ(item.documentation->value, "This is a property on the table!");
}

TEST_CASE_FIXTURE(FragmentAutocompleteFixture, "fragment_autocomplete_no_completions_in_single_line_comment_before_first_statement")
{
    auto oldSource = "local x = 1\n";

    auto [source, marker] = sourceWithMarker("-- TODO: |\nlocal x = 1\n");

    auto result = fragmentAutocomplete(oldSource, source, marker);
    CHECK(result.empty());
}

TEST_CASE_FIXTURE(FragmentAutocompleteFixture, "fragment_autocomplete_no_completions_in_block_comment_before_first_statement")
{
    auto oldSource = "local x = 1\n";

    auto [source, marker] = sourceWithMarker("--[[ TODO: | ]]\nlocal x = 1\n");

    auto result = fragmentAutocomplete(oldSource, source, marker);
    CHECK(result.empty());
}

TEST_CASE_FIXTURE(Fixture, "external_module_intersected_type_table_property_has_documentation")
{
    std::string typeSource = R"(
        export type A = {
            --- Example sick number
            Hello: number
        }

        export type B = {
            --- Example sick string
            Heya: string
        } & A
    )";

    auto [source, marker] = sourceWithMarker(R"(
        local bar = require("bar.luau")
        local item: bar.B = nil
        item.|
    )");

    auto typesUri = newDocument("bar.luau", typeSource);
    workspace.checkStrict(workspace.fileResolver.getModuleName((typesUri)), nullptr);

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    auto item = requireItem(result, "Hello");
    REQUIRE(item.documentation);
    CHECK_EQ(item.documentation->kind, lsp::MarkupKind::Markdown);
    trim(item.documentation->value);
    CHECK_EQ(item.documentation->value, "Example sick number");

    auto item2 = requireItem(result, "Heya");
    REQUIRE(item2.documentation);
    CHECK_EQ(item2.documentation->kind, lsp::MarkupKind::Markdown);
    trim(item2.documentation->value);
    CHECK_EQ(item2.documentation->value, "Example sick string");
}


TEST_CASE_FIXTURE(Fixture, "intersected_type_table_property_has_documentation")
{
    auto [source, marker] = sourceWithMarker(R"(
        type A = {
            --- Example sick number
            Hello: number
        }

        type B = {
            --- Example sick string
            Heya: string
        } & A

        local item: B = nil
        item.|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    auto item = requireItem(result, "Hello");
    REQUIRE(item.documentation);
    CHECK_EQ(item.documentation->kind, lsp::MarkupKind::Markdown);
    trim(item.documentation->value);
    CHECK_EQ(item.documentation->value, "Example sick number");

    auto item2 = requireItem(result, "Heya");
    REQUIRE(item2.documentation);
    CHECK_EQ(item2.documentation->kind, lsp::MarkupKind::Markdown);
    trim(item2.documentation->value);
    CHECK_EQ(item2.documentation->value, "Example sick string");
}

TEST_CASE_FIXTURE(Fixture, "type_reference_has_documentation")
{
    auto [source, marker] = sourceWithMarker(R"(
        --- Some documentation
        type SomeType = number

        local var: Some|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    auto typeEntry = getItem(result, "SomeType");
    REQUIRE(typeEntry);
    CHECK_EQ(typeEntry->kind, lsp::CompletionItemKind::Interface);
    REQUIRE(typeEntry->documentation);
    CHECK_EQ(typeEntry->documentation->kind, lsp::MarkupKind::Markdown);
    trim(typeEntry->documentation->value);
    CHECK_EQ(typeEntry->documentation->value, "Some documentation");
}

TEST_CASE_FIXTURE(Fixture, "imported_type_reference_has_documentation")
{
    newDocument("library.luau", R"(
        --- Some documentation
        export type SomeExportedType = number
        return {}
    )");

    auto [source, marker] = sourceWithMarker(R"(
        local library = require("library.luau")

        local var: library.Some|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    auto typeEntry = getItem(result, "SomeExportedType");
    REQUIRE(typeEntry);
    CHECK_EQ(typeEntry->kind, lsp::CompletionItemKind::Interface);
    REQUIRE(typeEntry->documentation);
    CHECK_EQ(typeEntry->documentation->kind, lsp::MarkupKind::Markdown);
    trim(typeEntry->documentation->value);
    CHECK_EQ(typeEntry->documentation->value, "Some documentation");
}

TEST_CASE_FIXTURE(Fixture, "deprecated_marker_in_documentation_comment_applies_to_autocomplete_entry")
{
    auto [source, marker] = sourceWithMarker(R"(
        --- @deprecated Use `bar` instead
        local function foo()
        end

        local x = |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = requireItem(result, "foo");
    CHECK(item.deprecated);
}

TEST_CASE_FIXTURE(Fixture, "deprecated_attribute_applies_to_autocomplete_entry")
{
    auto [source, marker] = sourceWithMarker(R"(
        @deprecated
        local function foo()
        end

        local x = |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = requireItem(result, "foo");
    CHECK(item.deprecated);
    CHECK_EQ(item.sortText, SortText::Deprioritized);
}

TEST_CASE_FIXTURE(Fixture, "deprecated_items_are_hidden_from_autocomplete_if_disabled")
{
    client->globalConfig.completion.showDeprecatedItems = false;
    auto [source, marker] = sourceWithMarker(R"(
        @deprecated
        local function foo()
        end

        local x = |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    CHECK_FALSE(getItem(result, "foo"));
}

TEST_CASE_FIXTURE(Fixture, "configure_properties_shown_when_autocompleting_index_with_colon")
{
    auto [source, marker] = sourceWithMarker(R"(
        local Foo = {}
        Foo.Value = 5

        function Foo:Bar()
        end

        local _ = Foo:|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    client->globalConfig.completion.showPropertiesOnMethodCall = true;
    auto result = workspace.completion(params, nullptr);
    CHECK(getItem(result, "Bar"));
    CHECK(getItem(result, "Value"));

    client->globalConfig.completion.showPropertiesOnMethodCall = false;
    result = workspace.completion(params, nullptr);
    CHECK(getItem(result, "Bar"));
    CHECK_FALSE(getItem(result, "Value"));
}

TEST_CASE_FIXTURE(Fixture, "variable_with_a_class_type_should_not_have_class_entry_kind_1")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local player: Instance = nil
        local x = p|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = requireItem(result, "player");
    CHECK_EQ(item.kind, lsp::CompletionItemKind::Variable);
}

TEST_CASE_FIXTURE(Fixture, "variable_with_a_class_type_should_not_have_class_entry_kind_2")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local function foo(player: Instance)
            local x = p|
        end
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = requireItem(result, "player");
    CHECK_EQ(item.kind, lsp::CompletionItemKind::Variable);
}

TEST_CASE_FIXTURE(Fixture, "string_completion_after_slash_should_replace_whole_string")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local tbl = {
            ["Item/Foo"] = 1,
            ["Item/Bar"] = 2,
            ["Item/Baz"] = 3,
        }

        tbl["Item/|"]
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    std::vector<std::string> labels{"Item/Foo", "Item/Bar", "Item/Baz"};
    for (const auto& label : labels)
    {
        auto item = requireItem(result, label);
        CHECK_EQ(item.kind, lsp::CompletionItemKind::Field);
        REQUIRE(item.textEdit);
        CHECK_EQ(item.textEdit->range.start, lsp::Position{8, 13});
        CHECK_EQ(item.textEdit->range.end, lsp::Position{8, 18});
        CHECK_EQ(item.textEdit->newText, label);
    }
}

TEST_CASE_FIXTURE(Fixture, "table_property_autocomplete_that_is_an_invalid_identifier_should_use_braces")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = {
            ["hello world"] = true
        }

        print(x.|)
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 1);
    auto item = requireItem(result, "hello world");
    CHECK_EQ(item.kind, lsp::CompletionItemKind::Field);
    REQUIRE(item.textEdit);
    CHECK_EQ(item.textEdit->range.start, lsp::Position{6, 16});
    CHECK_EQ(item.textEdit->range.end, lsp::Position{6, 16});
    CHECK_EQ(item.textEdit->newText, "[\"hello world\"]");

    REQUIRE_EQ(item.additionalTextEdits.size(), 1);
    CHECK_EQ(item.additionalTextEdits[0].range.start, lsp::Position{6, 15});
    CHECK_EQ(item.additionalTextEdits[0].range.end, lsp::Position{6, 16});
    CHECK_EQ(item.additionalTextEdits[0].newText, "");
}

TEST_CASE_FIXTURE(Fixture, "table_property_autocomplete_that_is_a_keyword_should_use_braces")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = {
            ["then"] = true
        }

        print(x.|)
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 1);
    auto item = requireItem(result, "then");
    CHECK_EQ(item.kind, lsp::CompletionItemKind::Field);
    REQUIRE(item.textEdit);
    CHECK_EQ(item.textEdit->range.start, lsp::Position{6, 16});
    CHECK_EQ(item.textEdit->range.end, lsp::Position{6, 16});
    CHECK_EQ(item.textEdit->newText, "[\"then\"]");

    REQUIRE_EQ(item.additionalTextEdits.size(), 1);
    CHECK_EQ(item.additionalTextEdits[0].range.start, lsp::Position{6, 15});
    CHECK_EQ(item.additionalTextEdits[0].range.end, lsp::Position{6, 16});
    CHECK_EQ(item.additionalTextEdits[0].newText, "");
}

static void checkStringCompletionExists(const std::vector<lsp::CompletionItem>& items, const std::string& label)
{
    auto item = requireItem(items, label);
    CHECK_EQ(item.kind, lsp::CompletionItemKind::Constant);
}

TEST_CASE_FIXTURE(Fixture, "instance_new_contains_creatable_instances")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        Instance.new("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 2);
    checkStringCompletionExists(result, "Part");
    checkStringCompletionExists(result, "TextLabel");
}

TEST_CASE_FIXTURE(Fixture, "get_service_contains_services")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        game:GetService("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 3);
    checkStringCompletionExists(result, "ReplicatedStorage");
    checkStringCompletionExists(result, "ServerScriptService");
    checkStringCompletionExists(result, "Workspace");
}

TEST_CASE_FIXTURE(Fixture, "instance_is_a_contains_classnames")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        Instance.new("Part"):IsA("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 9);
    checkStringCompletionExists(result, "Instance");
    checkStringCompletionExists(result, "BasePart");
    checkStringCompletionExists(result, "Part");
    checkStringCompletionExists(result, "TextLabel");
    checkStringCompletionExists(result, "ReplicatedStorage");
    checkStringCompletionExists(result, "ServerScriptService");
    checkStringCompletionExists(result, "Workspace");
    checkStringCompletionExists(result, "ServiceProvider");
    checkStringCompletionExists(result, "DataModel");
}

TEST_CASE_FIXTURE(Fixture, "enum_is_a_contains_enum_items")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        Enum.HumanoidRigType.R6:IsA("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 1);
    checkStringCompletionExists(result, "HumanoidRigType");
}

TEST_CASE_FIXTURE(Fixture, "get_property_changed_signal_includes_properties")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = Instance.new("Part")
        x:GetPropertyChangedSignal("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 4);
    checkStringCompletionExists(result, "Anchored");
    checkStringCompletionExists(result, "ClassName");
    checkStringCompletionExists(result, "Name");
    checkStringCompletionExists(result, "Parent");
}

TEST_CASE_FIXTURE(Fixture, "get_property_changed_signal_does_not_include_children_from_sourcemap")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "ReplicatedStorage",
                "className": "ReplicatedStorage"
            }
        ]
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        game:GetPropertyChangedSignal("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 3);
    CHECK_EQ(getItem(result, "ReplicatedStorage"), std::nullopt);
    checkStringCompletionExists(result, "Name");
    checkStringCompletionExists(result, "Parent");
    checkStringCompletionExists(result, "ClassName");
}

TEST_CASE_FIXTURE(Fixture, "get_property_changed_signal_does_not_include_children_from_sourcemap_second_level_getsourcemaptype_ty")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "ReplicatedStorage",
                "className": "ReplicatedStorage",
                "children": [{"name": "Part", "className": "Part"}]
            }
        ]
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        game.ReplicatedStorage:GetPropertyChangedSignal("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 3);
    CHECK_EQ(getItem(result, "Part"), std::nullopt);
    checkStringCompletionExists(result, "Name");
    checkStringCompletionExists(result, "Parent");
    checkStringCompletionExists(result, "ClassName");
}

TEST_CASE_FIXTURE(Fixture, "is_property_modified_includes_properties")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = Instance.new("Part")
        x:IsPropertyModified("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 4);
    checkStringCompletionExists(result, "Anchored");
    checkStringCompletionExists(result, "ClassName");
    checkStringCompletionExists(result, "Name");
    checkStringCompletionExists(result, "Parent");
}

TEST_CASE_FIXTURE(Fixture, "reset_property_to_default_includes_properties")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = Instance.new("Part")
        x:ResetPropertyToDefault("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 4);
    checkStringCompletionExists(result, "Anchored");
    checkStringCompletionExists(result, "ClassName");
    checkStringCompletionExists(result, "Name");
    checkStringCompletionExists(result, "Parent");
}

TEST_CASE_FIXTURE(Fixture, "find_first_child_on_datamodel_contains_children")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "ReplicatedStorage",
                "className": "ReplicatedStorage"
            },
            {
                "name": "StandardPart",
                "className": "Part"
            }
        ]
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        game:FindFirstChild("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 2);
    checkStringCompletionExists(result, "ReplicatedStorage");
    checkStringCompletionExists(result, "StandardPart");
}

TEST_CASE_FIXTURE(Fixture, "find_first_child_on_sourcemap_type_contains_children")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "StandardPart",
                "className": "Part",
                "children": [
                    {
                        "name": "ChildA",
                        "className": "Part"
                    },
                    {
                        "name": "ChildB",
                        "className": "Part"
                    }
                ]
            }
        ]
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        game.StandardPart:FindFirstChild("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 2);
    checkStringCompletionExists(result, "ChildA");
    checkStringCompletionExists(result, "ChildB");
}

TEST_CASE_FIXTURE(Fixture, "find_first_child_on_sourcemap_type_contains_children_second_level")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "StandardPart",
                "className": "Part",
                "children": [
                    {
                        "name": "ChildA",
                        "className": "Part",
                        "children": [{"name": "GrandChildA", "className": "Part"}, {"name": "GrandChildB", "className": "Part"}]
                    }
                ]
            }
        ]
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        game.StandardPart.ChildA:FindFirstChild("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 2);
    checkStringCompletionExists(result, "GrandChildA");
    checkStringCompletionExists(result, "GrandChildB");
}

TEST_CASE_FIXTURE(Fixture, "wait_for_child_on_datamodel_contains_children")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "ReplicatedStorage",
                "className": "ReplicatedStorage"
            },
            {
                "name": "StandardPart",
                "className": "Part"
            }
        ]
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        game:WaitForChild("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 2);
    checkStringCompletionExists(result, "ReplicatedStorage");
    checkStringCompletionExists(result, "StandardPart");
}

TEST_CASE_FIXTURE(Fixture, "wait_for_child_on_sourcemap_type_contains_children")
{
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "StandardPart",
                "className": "Part",
                "children": [
                    {
                        "name": "ChildA",
                        "className": "Part"
                    },
                    {
                        "name": "ChildB",
                        "className": "Part"
                    }
                ]
            }
        ]
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        game.StandardPart:WaitForChild("|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 2);
    checkStringCompletionExists(result, "ChildA");
    checkStringCompletionExists(result, "ChildB");
}

TEST_CASE_FIXTURE(Fixture, "auto_imports_handles_multi_line_existing_requires_when_adding_new_require_before")
{
    auto source = R"(
        local _ =
            require(script.Parent.d)
    )";
    auto astRoot = parse(source);
    auto uri = newDocument("foo.luau", source);
    auto textDocument = workspace.fileResolver.getTextDocument(uri);
    REQUIRE(textDocument);

    Luau::LanguageServer::AutoImports::RobloxFindImportsVisitor importsVisitor;
    importsVisitor.visit(astRoot);

    auto minimumLineNumber = computeMinimumLineNumberForRequire(importsVisitor, 0);
    auto insertedLineNumber = computeBestLineForRequire(importsVisitor, *textDocument, "script.Parent.c", minimumLineNumber);

    CHECK_EQ(insertedLineNumber, 1);
}

TEST_CASE_FIXTURE(Fixture, "auto_imports_handles_multi_line_existing_requires_when_adding_new_require_after")
{
    auto source = R"(
        local _ =
            require(script.Parent.d)
    )";
    auto astRoot = parse(source);
    auto uri = newDocument("foo.luau", source);
    auto textDocument = workspace.fileResolver.getTextDocument(uri);
    REQUIRE(textDocument);

    Luau::LanguageServer::AutoImports::RobloxFindImportsVisitor importsVisitor;
    importsVisitor.visit(astRoot);

    auto minimumLineNumber = computeMinimumLineNumberForRequire(importsVisitor, 0);
    auto insertedLineNumber = computeBestLineForRequire(importsVisitor, *textDocument, "script.Parent.e", minimumLineNumber);

    CHECK_EQ(insertedLineNumber, 3);
}

static void checkFileCompletionExists(const std::vector<lsp::CompletionItem>& items, const std::string& label, const std::string& insertText)
{
    auto item = requireItem(items, label);
    CHECK_EQ(item.kind, lsp::CompletionItemKind::File);
    CHECK_EQ(item.insertText, std::nullopt);
    REQUIRE(item.textEdit.has_value());
    CHECK_EQ(item.textEdit->newText, insertText);
}

static void checkFolderCompletionExists(const std::vector<lsp::CompletionItem>& items, const std::string& label, const std::string& insertText)
{
    auto item = requireItem(items, label);
    CHECK_EQ(item.kind, lsp::CompletionItemKind::Folder);
    CHECK_EQ(item.insertText, std::nullopt);
    REQUIRE(item.textEdit.has_value());
    CHECK_EQ(item.textEdit->newText, insertText);
}

TEST_CASE_FIXTURE(Fixture, "string_require_default_shows_initial_values")
{
    tempDir.write_child("first.luau", "return {}");
    tempDir.write_child("second.luau", "return {}");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("|")
    )");

    auto uri = newDocument(tempDir.write_child("source.luau", source), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 2);
    checkFolderCompletionExists(result, "..", "..");
    checkFolderCompletionExists(result, ".", ".");
}

TEST_CASE_FIXTURE(Fixture, "string_require_shows_files_in_current_directory")
{
    tempDir.write_child("first.luau", "return {}");
    tempDir.write_child("second.luau", "return {}");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("./|")
    )");

    auto uri = newDocument(tempDir.write_child("source.luau", source), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 4);
    checkFolderCompletionExists(result, "..", ".");
    checkFileCompletionExists(result, "source.luau", "./source");
    checkFileCompletionExists(result, "first.luau", "./first");
    checkFileCompletionExists(result, "second.luau", "./second");
}

TEST_CASE_FIXTURE(Fixture, "string_require_shows_files_in_parent_directory")
{
    tempDir.write_child("parent_first.luau", "return {}");
    tempDir.write_child("parent_second.luau", "return {}");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("../|")
    )");

    auto uri = newDocument(tempDir.write_child("nested/source.luau", source), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 4);
    checkFolderCompletionExists(result, "..", "../..");
    checkFolderCompletionExists(result, "nested", "../nested");
    checkFileCompletionExists(result, "parent_first.luau", "../parent_first");
    checkFileCompletionExists(result, "parent_second.luau", "../parent_second");
}

TEST_CASE_FIXTURE(Fixture, "string_require_shows_file_in_nested_directory")
{
    tempDir.write_child("sibling.luau", "return {}");
    tempDir.write_child("nested/child.luau", "return {}");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("./nested/|")
    )");

    auto uri = newDocument(tempDir.touch_child("source.luau"), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 2);
    checkFolderCompletionExists(result, "..", "./nested");
    checkFileCompletionExists(result, "child.luau", "./nested/child");
}

TEST_CASE_FIXTURE(Fixture, "string_require_shows_files_in_current_directory_after_entering_then_exiting_child_folder")
{
    tempDir.write_child("sibling.luau", "return {}");
    tempDir.write_child("nested/child.luau", "return {}");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("./nested/../|")
    )");

    auto uri = newDocument(tempDir.touch_child("source.luau"), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 4);
    checkFolderCompletionExists(result, "..", "./nested/../..");
    checkFolderCompletionExists(result, "nested", "./nested/../nested");
    checkFileCompletionExists(result, "source.luau", "./nested/../source");
    checkFileCompletionExists(result, "sibling.luau", "./nested/../sibling");
}

TEST_CASE_FIXTURE(Fixture, "string_require_contains_luaurc_aliases")
{
    loadLuaurc(R"(
    {
        "aliases": {
            "Roact": "roact",
            "Fusion": "fusion"
        }
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("|")
    )");

    auto uri = newDocument(tempDir.touch_child("source.luau"), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 4);
    checkFolderCompletionExists(result, "..", "..");
    checkFolderCompletionExists(result, ".", ".");
    requireItem(result, "@Roact");
    requireItem(result, "@Fusion");
}

TEST_CASE_FIXTURE(Fixture, "string_require_doesnt_show_aliases_after_a_directory_separator_is_seen")
{
    loadLuaurc(R"(
    {
        "aliases": {
            "Roact": "roact",
            "Fusion": "fusion"
        }
    })");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("test/|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    CHECK_EQ(result.size(), 1);
    checkFolderCompletionExists(result, "..", "test");
}

TEST_CASE_FIXTURE(Fixture, "string_require_shows_files_under_a_luaurc_directory_alias")
{
    tempDir.write_child("lune/process.luau", "return {}");
    tempDir.write_child("lune/net.luau", "return {}");

    std::string luaurc = R"(
    {
        "aliases": {
            "lune": "{PATH}",
        }
    })";
    replace(luaurc, "{PATH}", tempDir.path() + "/lune");
    loadLuaurc(luaurc);

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("@lune/|")
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 3);
    checkFolderCompletionExists(result, "..", "@lune");
    checkFileCompletionExists(result, "process.luau", "@lune/process");
    checkFileCompletionExists(result, "net.luau", "@lune/net");
}

TEST_CASE_FIXTURE(Fixture, "string_require_resolve_init_luau_relative_to_parent_directory")
{
    tempDir.write_child("project/sibling.luau", "return {}");
    tempDir.write_child("project/directory/utils.luau", "return {}");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("./|")
    )");

    auto uri = newDocument(tempDir.touch_child("project/directory/init.luau"), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 3);
    checkFolderCompletionExists(result, "..", ".");
    checkFolderCompletionExists(result, "directory", "./directory");
    checkFileCompletionExists(result, "sibling.luau", "./sibling");
}

TEST_CASE_FIXTURE(Fixture, "string_require_shows_self_alias_if_in_init_file")
{
    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("|")
    )");

    auto uri = newDocument("init.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    requireItem(result, "@self");
}

TEST_CASE_FIXTURE(Fixture, "string_require_resolves_self_alias_relative_to_current_file")
{
    tempDir.write_child("project/sibling.luau", "return {}");
    tempDir.write_child("project/directory/utils.luau", "return {}");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("@self/|")
    )");

    auto uri = newDocument(tempDir.touch_child("project/directory/init.luau"), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 2);
    checkFolderCompletionExists(result, "..", "@self");
    checkFileCompletionExists(result, "utils.luau", "@self/utils");
}

TEST_CASE_FIXTURE(Fixture, "string_require_does_not_show_files_matching_ignore_glob")
{
    client->globalConfig.completion.imports.ignoreGlobs = {"*.server.luau", "*.client.luau"};

    tempDir.write_child("project/main.server.luau", "return {}");
    tempDir.write_child("project/client_main.client.luau", "return {}");
    tempDir.write_child("project/utils.luau", "return {}");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("./|")
    )");

    auto uri = newDocument(tempDir.write_child("project/source.luau", source), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 3);
    checkFolderCompletionExists(result, "..", ".");
    checkFileCompletionExists(result, "source.luau", "./source");
    checkFileCompletionExists(result, "utils.luau", "./utils");
}

static std::vector<lsp::TextEdit> requireEndAutocompletionEdits(const TestClient* client, const Uri& uri)
{
    REQUIRE(!client->requestQueue.empty());
    auto request = client->requestQueue.back();
    REQUIRE_EQ(request.first, "workspace/applyEdit");
    REQUIRE(request.second);

    lsp::ApplyWorkspaceEditParams editParams = request.second.value();
    REQUIRE_EQ(editParams.edit.changes.size(), 1);

    return editParams.edit.changes[uri];
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_end_for_incomplete_function")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        function foo()
            |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 1);
    CHECK_EQ(edits[0].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[0].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_end_inside_of_function_call")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        call(function()
        |)
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 1);
    CHECK_EQ(edits[0].range, lsp::Range{{marker.line, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[0].newText, "\n        end)\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_then_in_if_statement_no_condition")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        if
|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 10}, {1, 10}});
    CHECK_EQ(edits[0].newText, " then");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_then_in_if_statement_with_condition")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        if condition
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 20}, {1, 20}});
    CHECK_EQ(edits[0].newText, " then");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_while_statement_no_condition")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        while
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 13}, {1, 13}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_while_statement_with_condition")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        while condition
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 23}, {1, 23}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_for_loop")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        for
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 11}, {1, 11}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_for_in_loop_no_values")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        for i in
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 16}, {1, 16}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_for_in_loop")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        for i in x
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 18}, {1, 18}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_numeric_for_loop")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        for i = 1, 10
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 21}, {1, 21}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_numeric_for_loop_with_step")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        for i = 1, 10, 2
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 24}, {1, 24}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_numeric_for_loop_missing_to")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        for i = 1,
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 18}, {1, 18}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_do_in_numeric_for_loop_missing_step")
{
    client->globalConfig.completion.autocompleteEnd = true;

    auto [source, marker] = sourceWithMarker(R"(
        for i = 1, 10,
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;
    params.context = lsp::CompletionContext{};
    params.context->triggerCharacter = "\n";

    auto result = workspace.completion(params, nullptr);
    auto edits = requireEndAutocompletionEdits(client.get(), uri);
    REQUIRE_EQ(edits.size(), 2);
    CHECK_EQ(edits[0].range, lsp::Range{{1, 22}, {1, 22}});
    CHECK_EQ(edits[0].newText, " do");
    CHECK_EQ(edits[1].range, lsp::Range{{marker.line + 1, 0}, {marker.line + 1, 0}});
    CHECK_EQ(edits[1].newText, "        end\n");
}

TEST_CASE_FIXTURE(Fixture, "dont_mark_type_as_function_kind_when_autocompleting_in_type_context")
{
    auto [source, marker] = sourceWithMarker(R"(
        export type Func = (string) -> string

        export type Mod = {
            apply: Fu|
        }
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = requireItem(result, "Func");
    CHECK_EQ(item.kind, lsp::CompletionItemKind::Interface);
    CHECK_EQ(item.labelDetails, std::nullopt);
    CHECK_EQ(item.insertText, std::nullopt);
    CHECK_EQ(item.textEdit, std::nullopt);
    CHECK_EQ(item.command, std::nullopt);
}

TEST_CASE_FIXTURE(Fixture, "completion_respects_cancellation")
{
    auto cancellationToken = std::make_shared<Luau::FrontendCancellationToken>();
    cancellationToken->cancel();

    auto document = newDocument("a.luau", "local x = 1");
    CHECK_THROWS_AS(workspace.completion(lsp::CompletionParams{{{document}}}, cancellationToken), RequestCancelledException);
}

static void enableSnippetSupport(lsp::ClientCapabilities& capabilities)
{
    capabilities.textDocument = lsp::TextDocumentClientCapabilities{};
    capabilities.textDocument->completion = lsp::CompletionClientCapabilities{};
    capabilities.textDocument->completion->completionItem = lsp::CompletionItemClientCapabilities{};
    capabilities.textDocument->completion->completionItem->snippetSupport = true;
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_inserts_param_names_in_parentheses_for_call")
{
    enableSnippetSupport(client->capabilities);

    auto [source, marker] = sourceWithMarker(R"(
        local function foo(x, y)
        end

        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto callEntry = getItem(result, "foo");

    REQUIRE(callEntry);
    REQUIRE(callEntry->insertText);
    CHECK_EQ(*callEntry->insertText, "foo(${1:x}, ${2:y})$0");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_does_not_include_optional_parameters_for_call")
{
    enableSnippetSupport(client->capabilities);

    auto [source, marker] = sourceWithMarker(R"(
        local function foo(x: string, y: number?)
        end

        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto callEntry = getItem(result, "foo");

    REQUIRE(callEntry);
    REQUIRE(callEntry->insertText);
    CHECK_EQ(*callEntry->insertText, "foo(${1:x})$0");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_puts_cursor_after_call_for_function_with_no_arguments")
{
    enableSnippetSupport(client->capabilities);

    auto [source, marker] = sourceWithMarker(R"(
        local function foo()
        end

        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto callEntry = getItem(result, "foo");

    REQUIRE(callEntry);
    REQUIRE(callEntry->insertText);
    CHECK_EQ(*callEntry->insertText, "foo()$0");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_still_puts_cursor_inside_of_call_if_there_are_arguments_but_they_are_all_optional")
{
    enableSnippetSupport(client->capabilities);

    auto [source, marker] = sourceWithMarker(R"(
        local function foo(x: string?, y: number?)
        end

        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    auto callEntry = getItem(result, "foo");
    REQUIRE(callEntry);
    REQUIRE(callEntry->insertText);
    CHECK_EQ(*callEntry->insertText, "foo($1)$0");

    // Should be the same for the require global
    auto requireEntry = getItem(result, "require");
    REQUIRE(requireEntry);
    REQUIRE(requireEntry->insertText);
    CHECK_EQ(*requireEntry->insertText, "require($1)$0");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_label_does_not_show_hidden_variadics")
{
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        local function isabsolute(path: string)
        end

        return isabs|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    auto func = getItem(result, "isabsolute");
    REQUIRE(func);
    REQUIRE(func->labelDetails);
    CHECK_EQ(func->labelDetails->detail, "(path)");
}

TEST_CASE_FIXTURE(Fixture, "prioritise_properties_when_sorting_autocomplete_in_table")
{
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        type Options = {
            name: string,
            description: string,
            label: string
        }
        local function create(options: Options)
        end

        create({
            |
        })
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    for (const auto property : {"name", "description", "label"})
    {
        auto entry = getItem(result, property);
        REQUIRE(entry);
        CHECK_EQ(entry->sortText, SortText::TableProperties);
    }
}

TEST_CASE_FIXTURE(Fixture, "prioritise_relevant_keywords_when_inside_of_if")
{
    ENABLE_NEW_SOLVER();

    auto [source, marker] = sourceWithMarker(R"(
        if true then
            |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    for (const auto property : {"else", "elseif", "end"})
    {
        auto entry = getItem(result, property);
        REQUIRE(entry);
        CHECK_EQ(entry->sortText, SortText::PrioritisedSuggestion);
    }
}

TEST_CASE_FIXTURE(Fixture, "do_not_show_keywords_if_disabled")
{
    client->globalConfig.completion.showKeywords = false;

    auto [source, marker] = sourceWithMarker(R"(
        |
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    for (const auto property : {"do", "export", "for", "function", "if", "local", "repeat", "return", "while"})
        CHECK_FALSE(getItem(result, property));
}

TEST_CASE_FIXTURE(Fixture, "show_anonymous_autofilled_function_by_default")
{
    auto [source, marker] = sourceWithMarker(R"(
        local function foo(cb: () -> ())
        end
        foo(|)
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    CHECK(getItem(result, "function (anonymous autofilled)"));
}

TEST_CASE_FIXTURE(Fixture, "do_not_show_anonymous_autofilled_function_if_disabled")
{
    client->globalConfig.completion.showAnonymousAutofilledFunction = false;

    auto [source, marker] = sourceWithMarker(R"(
        local function foo(cb: () -> ())
        end
        foo(|)
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    CHECK_FALSE(getItem(result, "function (anonymous autofilled)"));
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_documentation_for_property_on_union_type")
{
    auto source = R"(
        type BaseNode<HOS> = {
	        --[[
		        This is a documentation comment
	        ]]
	        has_one_supporter: HOS,
        }

        export type Node = BaseNode<true> | BaseNode<false>

        local x: Node = {} :: any

        x.
    )";

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = lsp::Position{14, 10};

    auto result = workspace.completion(params, nullptr);
    auto item = getItem(result, "has_one_supporter");
    REQUIRE(item);
    REQUIRE(item->documentation);
    CHECK_EQ(item->documentation->kind, lsp::MarkupKind::Markdown);
    trim(item->documentation->value);
    CHECK_EQ(item->documentation->value, "This is a documentation comment");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_documentation_for_base_table_property_on_setmetatable_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        local meta = {
            __index = {
                --- Documentation for prop_b.
                prop_b = "hello",
            }
        }

        local obj = setmetatable({
            --- Documentation for prop_a.
            prop_a = "world",
        }, meta)

        local y = obj.|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = getItem(result, "prop_a");
    REQUIRE(item);
    REQUIRE(item->documentation);
    CHECK_EQ(item->documentation->kind, lsp::MarkupKind::Markdown);
    trim(item->documentation->value);
    CHECK_EQ(item->documentation->value, "Documentation for prop_a.");
}

TEST_CASE_FIXTURE(Fixture, "autocomplete_documentation_for_index_property_on_setmetatable_type")
{
    auto [source, marker] = sourceWithMarker(R"(
        local meta = {
            __index = {
                --- Documentation for prop_b.
                prop_b = "hello",
            }
        }

        local obj = setmetatable({
            --- Documentation for prop_a.
            prop_a = "world",
        }, meta)

        local y = obj.|
    )");

    auto uri = newDocument("foo.luau", source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);
    auto item = getItem(result, "prop_b");
    REQUIRE(item);
    REQUIRE(item->documentation);
    CHECK_EQ(item->documentation->kind, lsp::MarkupKind::Markdown);
    trim(item->documentation->value);
    CHECK_EQ(item->documentation->value, "Documentation for prop_b.");
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_autocomplete_shows_datamodel_siblings")
{
    client->globalConfig.completion.imports.stringRequires.enabled = true;
    loadSourcemap(SOURCEMAP_FOR_STRING_REQUIRES);

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("./|")
    )");

    auto uri = newDocument(tempDir.write_child("packages/core/ModuleA.luau", source), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    checkFileCompletionExists(result, "ModuleB", "./ModuleB");
    checkFolderCompletionExists(result, "Nested", "./Nested");
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_autocomplete_shows_game_alias_children")
{
    client->globalConfig.completion.imports.stringRequires.enabled = true;
    loadSourcemap(SOURCEMAP_FOR_STRING_REQUIRES);

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("@game/|")
    )");

    auto uri = newDocument(tempDir.write_child("packages/core/ModuleA.luau", source), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    checkFolderCompletionExists(result, "ReplicatedStorage", "@game/ReplicatedStorage");
    checkFolderCompletionExists(result, "ServerScriptService", "@game/ServerScriptService");
}

TEST_CASE_FIXTURE(Fixture, "sourcemap_autocomplete_shows_self_alias_children")
{
    client->globalConfig.completion.imports.stringRequires.enabled = true;
    loadSourcemap(R"(
    {
        "name": "Game",
        "className": "DataModel",
        "children": [
            {
                "name": "Library",
                "className": "ModuleScript",
                "filePaths": ["lib/init.luau"],
                "children": [{"name": "Helper", "className": "ModuleScript", "filePaths": ["lib/Helper.luau"]}]
            }
        ]
    }
    )");

    tempDir.touch_child("lib/Helper.luau");

    auto [source, marker] = sourceWithMarker(R"(
        --!strict
        local x = require("@self/|")
    )");

    auto uri = newDocument(tempDir.write_child("lib/init.luau", source), source);

    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = marker;

    auto result = workspace.completion(params, nullptr);

    REQUIRE_EQ(result.size(), 2);
    checkFolderCompletionExists(result, "..", "@self");
    checkFileCompletionExists(result, "Helper.luau", "@self/Helper");
}

TEST_SUITE_END();
