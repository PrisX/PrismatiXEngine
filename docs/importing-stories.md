# Importing stories from other formats

The Authoring SDK provides the output side of a story importer. A format
adapter decodes its own source files and translates their commands into
PrismatiX Story lines. The SDK checks those lines, assigns the adapter's
explicit execution IDs to an adjacent `.pxids.json` sidecar, and assembles a
multi-scene, multi-locale `story.pxindex`. It does not decode KAG, SCN, PSB, or
another game's proprietary format by itself.

```ts
import {assembleImportedStoryProject} from "@prismatix/authoring-sdk";

const imported = assembleImportedStoryProject({
  id: "example-story",
  entryScene: "intro",
  locales: ["en"],
  chapters: [{id: "main", title: "Main"}],
  scenes: [{
    id: "intro", chapter: "main", requiredLabels: ["start"],
    sources: {en: {
      path: "Story/en/intro.pxstory",
      lines: [
        {source: "*start", id: "original.intro.start"},
        {source: "Hello.", id: "original.intro.greeting"},
        {source: "[end]", id: "original.intro.end"},
      ],
    }},
  }],
});
if (!imported.valid) throw new Error(JSON.stringify(imported.diagnostics));
// Publish imported.value.files together, then compile the project.
```

An adapter can use `serializeStoryCommand` for named JSON arguments and
`createImportedResourceResolver` for unambiguous media lookup. It should
decode one source scene at a time rather than retain all raw source data. The
assembler takes the translated scenes and returns canonical index, Story,
and sidecar text without writing files. Write the returned files as one
reviewable set; a failed import returns no set to publish.

Every executable line must have a stable ID assigned from the original
source. Blank lines, comments, speaker declarations, and `choice.wait` do not
take IDs. The SDK rejects duplicate IDs within a shared identity namespace,
different ID or operation-kind sequences across locales, missing locales,
unsafe or colliding paths, and source or sidecar files above 16 MiB. A later
`compileStoryProject` or `compileProject` must still check command meaning,
resources, labels, and cross-scene jumps. The assembler does not infer how
two versions of a story should map old saves; use the [save migration
tools](save-migrations.md) for that review.

The Senren port's SCN/PSB decoder and original-command translation remain
project-specific. They can adopt this output interface without making the
SDK claim general KAG or PSB input support.
