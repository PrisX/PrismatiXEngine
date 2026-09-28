# Resolving assets while importing Story

An importer can enumerate media once and use the Authoring SDK to match names
from an external script format to project-relative resource paths. Pass only
paths that belong to the resource category being imported:

```ts
import {createImportedResourceResolver} from "@prismatix/authoring-sdk";

const indexed = createImportedResourceResolver([
  "Assets/Characters/hero.png",
  "Assets/UI/hero.png",
  "Assets/BG/school.png",
]);
if (!indexed.valid || !indexed.value)
  throw new Error(JSON.stringify(indexed.diagnostics));

const image = indexed.value.resolve("hero.png");
if (image.kind === "ambiguous")
  throw new Error(`Choose an exact path: ${image.candidates.join(", ")}`);
if (image.kind !== "found") throw new Error(`Missing or unsafe image: ${image.query}`);
// Write image.path into a Story resource argument.
```

Exact project-relative paths win. A bare name is case-insensitive and matches
the filename stem; if the name includes an extension, that extension must also
match. Multiple candidates produce `ambiguous` with deterministic, sorted
paths. The resolver never selects one by file size, directory order, or locale.
It rejects unsafe index paths, and a query with a directory never falls back to
another directory's basename. If two source paths differ only by case, even an
exact lookup is ambiguous: this avoids a project that works on one filesystem
and opens a different asset on another.

The input is an array of **already enumerated** project-relative paths. The
resolver does not walk folders, decode external formats, create asset UUIDs, or
register assets in the package. Continue to use Story static resource
references, `resourceIncludes`, or the project asset catalog for build-time
dependencies. Keep the source format decoder in its importer and report every
missing or ambiguous lookup before writing generated Story files.
