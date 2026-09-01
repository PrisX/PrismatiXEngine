#include "Engine/SDK/RuntimeIr.h"

#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

int main() {
    {
        std::ifstream fixture(PRISMATIX_STORY_GOLDEN_IR_PATH,
                              std::ios::binary);
        assert(fixture.good());
        std::ostringstream text;
        text << fixture.rdbuf();
        const auto crossLanguage = px::sdk::ParseRuntimeIr(text.str());
        assert(crossLanguage.Valid());
        assert(crossLanguage.document.documentId ==
               "contract.story-golden");
        assert(crossLanguage.document.operations.size() == 4);
    }
    const std::string valid = R"({
      "format": "PrismatiXRuntimeIR",
      "schemaRevision": 2,
      "documentId": "scene-01",
      "committedRevision": 7,
      "operations": [{
        "operationId": "e1b61b69-8441-44f3-99c1-7232ce273187",
        "sourceId": "e1b61b69-8441-44f3-99c1-7232ce273187",
        "sourceLine": 23,
        "kind": "dialogue",
        "text": "@雪: 晚安🌙",
        "arguments": {"speaker":"雪", "text":"晚安🌙"}
      }]
    })";
    const auto parsed = px::sdk::ParseRuntimeIr(valid);
    assert(parsed.Valid());
    assert(parsed.document.documentId == "scene-01");
    assert(parsed.document.committedRevision == 7);
    assert(parsed.document.operations.size() == 1);
    assert(parsed.document.operations.front().operationId ==
           parsed.document.operations.front().sourceId);
    assert(parsed.document.operations.front().sourceLine == 23);

    const std::string duplicate = R"({
      "format": "PrismatiXRuntimeIR",
      "schemaRevision": 2,
      "documentId": "scene-01",
      "committedRevision": 8,
      "operations": [
        {"operationId":"same","sourceId":"source-a","kind":"scene","text":"a","arguments":{}},
        {"operationId":"same","sourceId":"source-b","kind":"dialogue","text":"b","arguments":{"text":"b"}}
      ]
    })";
    const auto rejected = px::sdk::ParseRuntimeIr(duplicate);
    assert(!rejected.Valid());
    assert(rejected.diagnostics.back().code == "PXSDKIR1015");

    const auto defaultLine = px::sdk::ParseRuntimeIr(R"({
      "format":"PrismatiXRuntimeIR", "schemaRevision":2,
      "documentId":"scene-01", "committedRevision":1,
      "operations":[
        {"operationId":"first","sourceId":"source-a","kind":"scene","text":"a","arguments":{}},
        {"operationId":"second","sourceId":"source-b","kind":"narration","text":"b","arguments":{"text":"b"}}
      ]
    })");
    assert(defaultLine.Valid());
    assert(defaultLine.document.operations.front().sourceLine == 1);
    assert(defaultLine.document.operations.back().sourceLine == 2);

    const auto invalidLine = px::sdk::ParseRuntimeIr(R"({
      "format":"PrismatiXRuntimeIR", "schemaRevision":2,
      "documentId":"scene-01", "committedRevision":1,
      "operations":[
        {"operationId":"first","sourceId":"source-a","sourceLine":0,
         "kind":"scene","text":"a","arguments":{}}
      ]
    })");
    assert(!invalidLine.Valid());
    assert(invalidLine.diagnostics.back().code == "PXSDKIR1019");

    const auto legacy = px::sdk::ParseRuntimeIr(R"({
      "format":"PrismatiXRuntimeIR", "schemaRevision":1,
      "documentId":"scene-01", "committedRevision":1, "operations":[]
    })");
    assert(!legacy.Valid());

    const auto structural = px::sdk::ParseRuntimeIr(R"({
      "format":"PrismatiXRuntimeIR", "schemaRevision":2,
      "documentId":"scene-01", "committedRevision":1,
      "operations":[
        {"operationId":"structure","sourceId":"source-a","kind":"sequence",
         "text":"structure","arguments":{}}
      ]
    })");
    assert(!structural.Valid());
    assert(structural.diagnostics.back().code == "PXSDKIR1021");

    const auto unknownField = px::sdk::ParseRuntimeIr(R"({
      "format":"PrismatiXRuntimeIR", "schemaRevision":2,
      "documentId":"scene-01", "committedRevision":1, "editorState":{},
      "operations":[]
    })");
    assert(!unknownField.Valid());
    assert(unknownField.diagnostics.back().code == "PXSDKIR1008");

    const std::string oversized(16 * 1024 * 1024 + 1, 'x');
    const auto bounded = px::sdk::ParseRuntimeIr(oversized);
    assert(!bounded.Valid());
    assert(bounded.diagnostics.back().code == "PXSDKIR1007");
    return 0;
}
