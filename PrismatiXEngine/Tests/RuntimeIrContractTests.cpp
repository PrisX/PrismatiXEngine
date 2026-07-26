#include "Engine/SDK/RuntimeIr.h"

#include <cassert>
#include <string>

int main() {
    const std::string valid = R"({
      "format": "PrismatiXRuntimeIR",
      "schemaRevision": 1,
      "documentId": "scene-01",
      "committedRevision": 7,
      "operations": [{
        "operationId": "e1b61b69-8441-44f3-99c1-7232ce273187",
        "sourceId": "e1b61b69-8441-44f3-99c1-7232ce273187",
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

    const std::string duplicate = R"({
      "format": "PrismatiXRuntimeIR",
      "schemaRevision": 1,
      "documentId": "scene-01",
      "committedRevision": 8,
      "operations": [
        {"operationId":"same","sourceId":"source-a","kind":"scene","text":"a","arguments":{}},
        {"operationId":"same","sourceId":"source-b","kind":"sequence","text":"b","arguments":{}}
      ]
    })";
    const auto rejected = px::sdk::ParseRuntimeIr(duplicate);
    assert(!rejected.Valid());
    assert(rejected.diagnostics.back().code == "PXSDKIR1015");

    const auto future = px::sdk::ParseRuntimeIr(R"({
      "format":"PrismatiXRuntimeIR", "schemaRevision":2,
      "documentId":"scene-01", "committedRevision":1, "operations":[]
    })");
    assert(!future.Valid());
    return 0;
}
