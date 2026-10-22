# Sharing extensions

An extension (a `.pxextension` manifest and its script, see
[Extension commands](story-reference.md#extension-commands)) is shared as one
`.pxext` file. You pack it, publish it to a registry or host the file
yourself, and other projects install it with `prismatix extension add`. This
page covers each step, the `.pxext` format, the registry HTTP API a registry
implements, the static index layout, the lock file and the security model.

- [Authoring and packing](#authoring-and-packing)
- [Publishing and hosting](#publishing-and-hosting)
- [Installing](#installing)
- [The lock file](#the-lock-file)
- [The .pxext format](#the-pxext-format)
- [Registry HTTP API v1](#registry-http-api-v1)
- [Static index](#static-index)
- [Security model](#security-model)

An extension with native libraries also ships one native pack per platform,
installs only with `--allow-native` and never builds for the Web:
[Low-level and native extensions](native-extensions.md) covers what that
adds to every section below.

## Authoring and packing

A shared extension has an id `owner/name`. The owner is your registry user or
organization (lowercase letters, digits and single dashes, at most 39
characters); the name is the manifest's `id`, which for a shared extension
must be lowercase (letters, digits, `.`, `_`, `-`, at most 64 characters).

```sh
prismatix extension create glow --command shine
prismatix extension pack glow --owner owen
```

The first `pack` with `--owner` writes `pxext.json` beside the manifest, and a
`README.md` if there is none. Fill them in before you publish:

```json
{
  "format": "PrismatiXExtensionPackage",
  "formatVersion": 1,
  "id": "owen/glow",
  "displayName": "Glow",
  "description": "Soft light effects for Story scenes.",
  "author": {"name": "Owen", "url": "https://example.com"},
  "license": "MIT",
  "homepage": "https://example.com/glow",
  "repository": "https://github.com/owen/glow",
  "keywords": ["effects", "light"],
  "icon": "icon.png",
  "screenshots": ["media/scene.png"]
}
```

Only `format`, `formatVersion` and `id` are required
(`Contracts/extension-package.schema.json`). `icon` is a PNG of at most
256 KiB (256×256 is best); `screenshots` are up to 8 PNG, JPEG or WebP files
of at most 2 MiB. Paths are relative to the extension's folder. Put a
`LICENSE` file there too; pack warns without one.

`pack` writes `dist/extensions/<owner>-<name>-<version>.pxext` (`--output
<dir>` elsewhere). The version is the manifest's `version`; raise it for
every release. pack:

- bundles a TypeScript extension and type-checks it exactly like a build
  (errors stop it; type errors and host calls whose capability the manifest
  does not declare are warnings, `PXCLIEXT2001` and `PXCLIEXT2004`), or copies
  a JavaScript entry and its `modules` and warns about undeclared
  capabilities;
- checks the manifest and `pxext.json` against their contracts and the
  package against the size limits below;
- adds `types/prismatix-extension.d.ts` (the handler types generated from the
  manifest), the README, LICENSE, CHANGELOG.md, icon and screenshots;
- with `--include-source`, adds the extension's folder under `src/` (without
  `node_modules`, `dist`, hidden files and `.pxext` files).

The output is reproducible: the same sources give the same bytes and the same
SHA-256 on any computer, so anyone can rebuild a published version from its
source and compare.

## Publishing and hosting

There are three ways to share a package.

**A registry.** With a registry that implements the
[API below](#registry-http-api-v1):

```sh
export PRISMATIX_EXTENSION_REGISTRY=https://registry.example.com
export PRISMATIX_REGISTRY_TOKEN=...          # your publish token
prismatix extension publish glow             # packs, then publishes
prismatix extension publish dist/extensions/owen-glow-1.0.0.pxext
prismatix extension yank owen/glow@1.0.1 --reason "breaks rollback"
prismatix extension yank owen/glow@1.0.1 --undo
```

The token is read from `PRISMATIX_REGISTRY_TOKEN` only; it is never written to
the project. A registry refuses a version it already has (`version_exists`):
publish a new version instead. Yanking keeps a version installable for
projects that pin it exactly, but no range resolves to it.

**A static index** (GitHub Pages or any web host): see
[Static index](#static-index).

**The file itself**: attach the `.pxext` to a release, or put it on npm. An
npm package holds one `.pxext` at its root, or names it in `package.json`:
`"prismatix": {"extension": "dist/owner-name-1.0.0.pxext"}`.

There is no default registry. The CLI uses, in order, `--registry <url>`, the
`PRISMATIX_EXTENSION_REGISTRY` environment variable, and the project's
`settings["extensions.registry"]`, which `prismatix extension registry <url>`
sets (`prismatix extension registry none` removes it; without an argument it
shows the one in use). Registry, download and upload URLs must be `https`;
`http` is accepted only for `localhost`, `127.0.0.1` and `[::1]`.

## Installing

```sh
prismatix extension add owen/glow                 # newest version for this engine
prismatix extension add owen/glow@^1.2            # a SemVer range
prismatix extension add ./owen-glow-1.0.0.pxext
prismatix extension add https://example.com/owen-glow-1.0.0.pxext --sha256 <hex>
prismatix extension add npm:prismatix-glow@^1
prismatix extension inspect owen/glow             # what it may do, without installing
```

`add` downloads the package, checks its size and SHA-256 against the registry
(or `--sha256`), checks the archive, `pxext.json` and the manifest, refuses a
version whose `requiredEngineVersion` does not include this project's engine
(the version of the project's CLI) and refuses a package whose manifest id or
any command or Action id is already declared by another of the project's
extensions. It then shows what the extension may do (its capabilities, its
commands and Actions) and asks before installing; `--yes` answers for scripts
and CI, which never prompt. Nothing from the package runs.

The package is extracted into `Extensions/<owner>/<name>/`, its manifest
(`Extensions/<owner>/<name>/<name>.pxextension`) is added to the project's
`extensions`, and `prismatix-extensions.lock` records what was installed.
Commit all three: a teammate's checkout needs no network to build.

Ranges are npm-style: `^1.2.0`, `~1.2`, `1`, `1.x`, `>=1.0.0 <2.0.0`,
`1.0.0 || ^2`, `*`. Without a range, `add` takes the newest version for this
engine and locks `^<that version>`. Yanked versions are skipped unless the
range is that exact version (`owner/name@1.0.1`), which installs with a
warning. Prereleases (`1.1.0-beta.1`) match only ranges that name a
prerelease.

```sh
prismatix extension list                   # every extension, with source, version and lock status
prismatix extension outdated               # current, wanted (locked range) and latest, for this engine
prismatix extension update                 # every registry and npm install, within its range
prismatix extension update owen/glow --to ^2
prismatix extension verify                 # re-hash installed files against the lock; exit 1 on a change
prismatix extension update owen/glow --reinstall   # restore changed files: the locked version again
prismatix extension remove owen/glow
```

`update` asks again (or needs `--yes`) when the new version declares a
capability the installed one did not. Packages installed from a file or URL
are not updated; add the newer file again. `verify` also reports registered
`Extensions/` manifests the lock does not list and command or Action ids two
extensions both declare.

Every subcommand takes `--json` and prints one JSON result (PrismatiX Studio
runs them this way); progress and warnings go to stderr as JSON records.

## The lock file

`prismatix-extensions.lock` (`Contracts/extension-lock.schema.json`), sorted
and stable for diffs:

```json
{
  "format": "PrismatiXExtensionLock",
  "lockVersion": 1,
  "extensions": {
    "owen/glow": {
      "version": "1.2.0",
      "sha256": "<SHA-256 of the .pxext>",
      "size": 18233,
      "source": "registry+https://registry.example.com",
      "range": "^1.2.0",
      "manifest": "Extensions/owen/glow/glow.pxextension",
      "files": "<digest of the installed files>"
    }
  }
}
```

`source` is `registry+<url>`, `file:<path>` (relative to the project, or the
file name for a file elsewhere), the `https` URL, or `npm:<package>@<version>`.
`range` is what `update` follows (registry and npm installs). `files` is the
SHA-256 of the lines `<sha256>  <path>\n` (sha256sum's format) of every file
in `Extensions/<owner>/<name>/`, sorted by path; `verify` recomputes it, so an
added, removed or edited file shows as `modified`.

## The .pxext format

A `.pxext` is a ZIP archive in one canonical form
(`writeExtensionPackage` in `@prismatix/authoring-sdk` writes it, and only
that form is accepted):

- every entry is a regular file (no directory entries, links or special
  files), stored without compression (method 0), with the UTF-8 name flag and
  no other flags, no extra fields, comments, data descriptors, encryption,
  ZIP64 or archive comment;
- entries are sorted by path, contiguous from offset 0, followed directly by
  the central directory and the end record; the local and central records
  agree and every CRC-32 is checked;
- times are 1980-01-01 00:00 and permissions `0644` (made by Unix, ZIP 2.0);
- paths are `/`-separated segments of `A-Z a-z 0-9 _ - .` that do not start
  with `.` or `-`, do not end with `.`, are not a Windows device name (`CON`,
  `NUL`, `COM1`, ...), at most 100 characters each and 200 in all; no two
  paths differ only in case.

Storing without compression keeps packages byte-for-byte reproducible on any
computer and makes a zip bomb impossible (a package is never larger on disk
than in transit); HTTP compression shrinks the download. Ordinary zip tools add
timestamps and extra fields, so build packages with `prismatix extension pack`.

Contents:

| Path | |
|---|---|
| `pxext.json` | Required. Package metadata (above) with `version` and `manifest` filled in by pack. |
| `<name>.pxextension` | Required. The manifest: `language` `javascript`, `entry` and `modules` in `dist/`, `id` = name, `version` = `pxext.json` version, a valid `requiredEngineVersion` range. |
| `dist/**/*.js` | Required: the entry (and modules). For TypeScript, the bundle. |
| `types/**/*.d.ts` | Handler types generated from the manifest. |
| `README.md`, `LICENSE`, `LICENSE.md`, `LICENSE.txt`, `CHANGELOG.md` | Optional (pack warns without a README or LICENSE). |
| `icon.png` | Only when `pxext.json` names it. |
| `screenshots/<file>` | Only the files `pxext.json` lists. |
| `src/**` | Optional source (`--include-source`). |

Any other file makes the package invalid. Limits: 32 MiB per package, 2048
files, 16 MiB of `dist/`, 1 MiB manifest, 64 KiB `pxext.json`.

`validateExtensionPackage(bytes)` (also importable alone as
`@prismatix/authoring-sdk/extension-package`, which uses no Node.js APIs:
`Uint8Array`, `TextDecoder` and Web Crypto only) runs every check above and
returns `{valid, diagnostics, sha256, size, id, version, metadata, manifest,
files}`. Errors are `PXEXTPKG1xxx`, warnings `PXEXTPKG2xxx`:

| Code | Meaning |
|---|---|
| PXEXTPKG1001 | Not a canonical .pxext archive (damaged, gaps, overlaps, trailing data) |
| PXEXTPKG1002 | A size or count limit is exceeded |
| PXEXTPKG1003 | An unsafe path (`..`, absolute, backslash, not UTF-8, ...) |
| PXEXTPKG1004 | Two paths differ only in case |
| PXEXTPKG1005 | A directory, link, compressed, encrypted or extra-field entry |
| PXEXTPKG1006 | A CRC-32 mismatch |
| PXEXTPKG1007 | `pxext.json` is missing or not JSON |
| PXEXTPKG1008 | `pxext.json` does not match its contract |
| PXEXTPKG1009 | The manifest is missing, misnamed or invalid |
| PXEXTPKG1010 | The manifest disagrees with the package (id, version, language, scripts) |
| PXEXTPKG1011 | A file a package may not hold |
| PXEXTPKG1012 | A bad icon or screenshot |
| PXEXTPKG1013 | `requiredEngineVersion` is not a SemVer range |
| PXEXTPKG1014 | Entries are not in path order |
| PXEXTPKG1015 | A native pack is missing a file its manifest lists, or holds one it does not |
| PXEXTPKG1016 | A native pack's file does not match the SHA-256 its manifest pins |
| PXEXTPKG2001 / 2002 / 2003 | No README.md / no LICENSE file / no `license` in `pxext.json` |
| PXEXTPKG2004 | A `dist/` script the manifest does not load |

## Registry HTTP API v1

A registry serves these routes under its base URL. Bodies are JSON
(`Contracts/extension-registry.schema.json`, `validateRegistryDocument` in the
SDK; the TypeScript types are `RegistrySearchResponse`, `RegistryExtension`,
`RegistryVersion`, ...). Clients ignore unknown properties. URLs in a document
(`download`, `icon`, `screenshots`, `uploadUrl`) may be relative; they resolve
against the URL of the document that holds them. Ids are `owner/name`; in a
path they are two segments.

Errors are `{"error": {"code": "<code>", "message": "<text>"}}` with a 4xx/5xx
status. A 404 **with** this body is an answer (`not_found`,
`no_matching_version`, ...); a 404 without it means the route does not exist,
and the client tries the route's static file (below).

### GET /v1/search?q=&engine=&capability=&page=

`q` matches id, display name, description and keywords; `engine` (the
client's engine version) keeps extensions with a non-yanked version for that
engine, and then `latestVersion` and `requiredEngineVersion` describe the
newest such version; `capability` keeps extensions that declare it; `page`
starts at 1.

```json
{
  "query": "glow",
  "results": [{
    "id": "owen/glow", "owner": "owen", "name": "glow",
    "displayName": "Glow", "description": "Soft light effects for Story scenes.",
    "latestVersion": "1.2.0", "requiredEngineVersion": ">=0.1.0 <0.2.0",
    "capabilities": ["runtime"], "keywords": ["effects"],
    "icon": "https://registry.example.com/icons/owen/glow.png",
    "downloads": 120, "updatedAt": "2026-10-22T00:00:00Z"
  }],
  "total": 1, "page": 1, "pageSize": 20
}
```

Required in a result: `id`, `latestVersion`, `requiredEngineVersion`,
`capabilities`.

### GET /v1/extensions/{owner}/{name}

```json
{
  "id": "owen/glow", "owner": "owen", "name": "glow",
  "displayName": "Glow", "description": "...",
  "author": {"name": "Owen", "url": "https://example.com"},
  "license": "MIT", "homepage": "https://...", "repository": "https://...",
  "keywords": ["effects"], "icon": "icon.png", "screenshots": ["screenshots/scene.png"],
  "capabilities": ["runtime"],
  "latestVersion": "1.2.0",
  "versions": [
    {"version": "1.2.0", "requiredEngineVersion": ">=0.1.0 <0.2.0", "yanked": false,
     "sha256": "...", "size": 18233, "publishedAt": "2026-10-22T00:00:00Z"},
    {"version": "1.1.0", "requiredEngineVersion": ">=0.1.0 <0.2.0", "yanked": true}
  ],
  "downloads": 120, "createdAt": "...", "updatedAt": "..."
}
```

Required: `id`, `capabilities`, `versions` (each with `version`,
`requiredEngineVersion`, `yanked`), newest first. `latestVersion` is the
newest non-yanked, non-prerelease version, or `null`. The metadata comes from
the latest version's `pxext.json`.

### GET /v1/extensions/{owner}/{name}/versions/{version}

```json
{
  "id": "owen/glow", "version": "1.2.0",
  "requiredEngineVersion": ">=0.1.0 <0.2.0",
  "sha256": "<lowercase hex SHA-256 of the .pxext>", "size": 18233,
  "download": "https://cdn.example.com/owen-glow-1.2.0.pxext",
  "manifest": { "...": "the package's .pxextension" },
  "metadata": { "...": "the package's pxext.json" },
  "yanked": false, "yankedReason": "...", "publishedAt": "2026-10-22T00:00:00Z"
}
```

Required: `version`, `requiredEngineVersion`, `sha256`, `size`, `download`,
`manifest`, `yanked`. The client refuses a download whose size or SHA-256
differs, and a package whose `pxext.json` id or version differs from the one
asked for. Downloads may redirect (to a CDN or a presigned URL) but must end
on `https`.

### GET /v1/extensions/{owner}/{name}/resolve?range=&engine=

Returns the version document (as above) of the newest version that

1. satisfies `range` (npm-style; `*` when absent),
2. is not yanked, unless `range` is exactly that version,
3. is not a prerelease, unless `range` names a prerelease,
4. has a `requiredEngineVersion` that includes `engine` (when given).

When none does: 404 `{"error": {"code": "no_matching_version", ...}}` (or
`engine_incompatible` when versions match the range but not the engine).
`resolveExtensionVersion(versions, range, engine)` in the SDK is this rule.

### POST /v1/extensions (publish)

`Authorization: Bearer <token>`. Body (`publishRequest`):

```json
{"id": "owen/glow", "version": "1.2.0", "sha256": "...", "size": 18233,
 "manifest": { "...": "" }, "metadata": { "...": "" }}
```

Answers 201 (`publishResponse`) with where to upload the file:

```json
{"uploadId": "u_8f2c", "uploadUrl": "https://storage.example.com/...?X-Signature=...",
 "uploadMethod": "PUT", "uploadHeaders": {"x-amz-checksum-sha256": "..."},
 "expiresAt": "2026-10-22T00:15:00Z"}
```

or 401 `unauthorized`, 403 `forbidden` (not this owner), 409
`version_exists`, 422 `invalid_package`. The client then sends `PUT
uploadUrl` with the bytes, `content-type: application/octet-stream` and the
`uploadHeaders` (never the token), and confirms:

### POST /v1/uploads/{uploadId}/confirm

`Authorization: Bearer <token>`, no body. The registry fetches the uploaded
file, checks it with `validateExtensionPackage` and against the announced id,
version, size and SHA-256, publishes it, and answers 200 with the version
document. The client checks that the confirmed `version` and `sha256` are the
ones it uploaded.

### POST /v1/extensions/{owner}/{name}/versions/{version}/yank

`Authorization: Bearer <token>`. Body `{"yanked": true, "reason": "..."}`
(`"yanked": false` restores it). Answers 200 with the version document.

## Static index

A registry can start as plain files: the GET routes, each as
`<route>/index.json`. The client asks for the route first; on a 404 without a
JSON error body, or an HTML page (a host listing the route's folder), it
fetches `<route>/index.json`, resolves versions itself from
the extension document (the rule above), and filters the search file by `q`
and `capability` (a static search lists each extension's latest version and
does not filter by engine; installing still resolves a compatible version).

```
v1/search/index.json                                 {"results": [...], "total", "page": 1, "pageSize"}
v1/extensions/<owner>/<name>/index.json              the extension document
v1/extensions/<owner>/<name>/icon.png                (when the package has one)
v1/extensions/<owner>/<name>/screenshots/<file>
v1/extensions/<owner>/<name>/versions/<version>/index.json
```

`scripts/extension-index.mjs` builds this tree. It needs only
`@prismatix/authoring-sdk`, so an index repository's CI can run it (and
publish the output to GitHub Pages):

```sh
node scripts/extension-index.mjs entries site
```

`entries/` holds one JSON file per extension:

```json
{"id": "owen/glow", "versions": [
  {"url": "https://github.com/owen/glow/releases/download/v1.2.0/owen-glow-1.2.0.pxext",
   "sha256": "<optional pin>", "publishedAt": "2026-10-22T00:00:00Z"},
  {"url": "https://.../owen-glow-1.1.0.pxext", "yanked": true, "yankedReason": "breaks rollback"}
]}
```

The script downloads every package, runs `validateExtensionPackage`, checks
the id and the pinned SHA-256, takes the version and metadata from the
package, and writes the documents with `download` pointing at the original
URL. A `url` relative to the entry file (a package kept in the index
repository) is copied next to its version document. Contributors add an
extension with a pull request that adds or edits its entry file; the CI
re-runs the script, which fails on any invalid package.

## Security model

- **Installing runs nothing.** `add`, `update` and `inspect` only read the
  package: no install scripts, no hooks, no `npm install`. An npm source is
  fetched as a tarball (its npm integrity checked) and unpacked to a temporary
  folder only to take the `.pxext` out of it.
- **Integrity.** Registry versions are checked against their SHA-256 and size
  before anything is read; file and URL sources can be pinned with
  `--sha256`. The lock records each package's SHA-256 and a digest of its
  installed files, and `verify` re-checks them, so a change to
  `Extensions/` shows up in review and CI.
- **Safe extraction.** Only the canonical archive form is accepted, so there
  are no `..` or absolute paths, links, device names, case collisions,
  compression bombs or hidden entries; size and count limits apply before
  writing. Files are written to a staging folder inside `Extensions/` and
  moved into place only when all of them are written; a failure leaves the
  previous install as it was.
- **Transport.** Registry, download and upload URLs are `https` (`http` only
  on this computer). The publish token comes from `PRISMATIX_REGISTRY_TOKEN`,
  is sent only to the registry, never to the upload URL, and is never stored.
- **Consent.** Before installing, the CLI (and Studio) shows the
  capabilities the extension declares and its commands and Actions; an
  update that adds a capability asks again.
- **Native code** ([details](native-extensions.md)) is the exception: a
  library runs in the game process with the player's permissions, so it
  installs only with `--allow-native`, is signed per platform, is pinned by
  SHA-256 in the lock and the packaged manifest, and is checked again by the
  Player before it loads.
- **At run time** an extension's script runs only inside the game's sandboxed
  JavaScript host (QuickJS): no Node.js, no file system or network, no
  `Date` or `Math.random`, and the Player refuses host calls outside the
  manifest's `capabilities`. A capability is a promise about what the script
  may ask the engine to do, not a code review: install extensions from people
  you trust, and read their source (packages may include it under `src/`, and
  a reproducible pack lets you rebuild a version and compare its SHA-256).
- **Conflicts.** An extension whose id or a command/Action id is already
  declared by another of the project's extensions is not installed, so an
  installed package cannot take over another extension's commands.

PrismatiX Studio's Extensions workspace runs the same commands (export,
drag-and-drop install with the capability prompt, update, remove and the
Explore tab).
