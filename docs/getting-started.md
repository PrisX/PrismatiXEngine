# Build your first PrismatiX game

This guide is for story authors. You need Node.js 20 or newer, the PrismatiX CLI, and a PrismatiX runtime bundle for your platform. You do not need a C++ compiler, CMake, vcpkg, a source checkout, or a globally installed PrismatiX tool.

Released CLI packages may include the matching runtime. If yours does not, extract a same-version runtime bundle and set `PRISMATIX_RUNTIME_DIR` to its top-level directory. You can instead pass `--runtime <directory>` to `dev`, `build`, `pack`, or `run`. A runtime directory may be an install prefix, its `bin` directory, its `runtime-manifest.json`, or the directory containing that manifest.

## The five-step author workflow

### 1. Install the authoring tool

Start in any empty directory outside the PrismatiXEngine repository:

```bash
npm init -y
npm install --save-dev @prismatix/cli
```

The project-local install keeps the CLI version with the game. All commands below use `npx`, so no global npm package is required.

### 2. Create a game

```bash
npx prismatix create first-game --name "First Light" --locale en
cd first-game
```

This creates a playable starter with a default title screen, game catalog, locale, and Story index. The CLI generates its canonical metadata and UUIDs; a first game does not require you to hand-author them.

### 3. Write one line

Open `Story/main.pxstory` and change the welcome text:

```pxstory
[id main.welcome]
The station lights came on one by one.

[id main.finish]
[end]
```

You may also add assets under `Assets/` and refer to them by project-relative path. Stable asset identities and custom UI are available when a project needs them, but they are not onboarding prerequisites.

### 4. Preview while you edit

```bash
npx prismatix dev
```

`dev` compiles the canonical Runtime IR, starts PreviewHost, and watches project dependencies. Saving the Story file patches or restarts Preview as required. Diagnostics are emitted immediately as NDJSON with a stable code, source path, and line/column span, so editors and CI can locate the authoring error.

For a non-interactive environment, use the same handshake and initial apply/play path once:

```bash
npx prismatix dev --once
```

Press Ctrl+C to stop a normal watch session.

### 5. Build the portable game

```bash
npx prismatix build
```

With no `--output` or `--artifact-only` option, `build` validates, compiles, packages the matching Player and native dependencies, and writes a distribution under `dist/<project-id>/`. Run the Player from that directory; the distribution must not read the engine repository or an author-machine vcpkg installation.

Use `prismatix build --artifact-only` when a CI pipeline only needs canonical Runtime inputs, or `prismatix pack` when you want to package explicitly. `prismatix run` packages and launches in one command.

## Runtime discovery

Normally no option is needed. When selecting a standalone runtime bundle, prefer an explicit project or CI setting:

```bash
npx prismatix dev --runtime /opt/prismatix-0.2
npx prismatix build --runtime /opt/prismatix-0.2
```

PowerShell uses the same CLI syntax:

```powershell
$env:PRISMATIX_RUNTIME_DIR = "C:\Tools\PrismatiX-0.2"
npx prismatix dev --once
```

Tool lookup is deterministic: a per-tool option, `--runtime`, a per-tool environment variable, `PRISMATIX_RUNTIME_DIR`, a platform runtime bundled with the CLI, `PATH`, then source-checkout developer fallbacks. A missing tool reports every checked location and tells you which option or environment variable to set.

The runtime manifest lives at `share/PrismatiX/runtime-manifest.json` and names the matching PreviewHost, Packager, and Player. Keep the CLI and runtime bundle on the same released version.

## Validate and diagnose

Run validation without launching Preview:

```bash
npx prismatix validate
```

A diagnostic is one JSON object per stderr line. For example, an unknown Story command reports `PXSTORY1199` and a span similar to `Story/main.pxstory:5`. Do not discard stderr in editor integrations.

## Verify a runtime bundle release

Engine release jobs have two Phase 1 gates:

```bash
# Fast: packs and installs the real npm artifacts in a temporary, repo-external
# author project and exercises the workflow with protocol-compatible mock tools.
npm run test:phase1:mock

# Release: repeats the clean journey with a redistributable native bundle and
# launches the packaged Player from outside the repository.
PRISMATIX_PHASE1_RUNTIME=/path/to/install-prefix npm run test:phase1
```

On PowerShell:

```powershell
$env:PRISMATIX_PHASE1_RUNTIME = "C:\Packages\PrismatiX-runtime"
npm run test:phase1
```

The release gate copies the supplied bundle out of the checkout, strips repository/vcpkg discovery variables and paths, verifies source diagnostics, runs `dev --once`, exercises both `build` and `pack`, and keeps the failed temporary workspace for inspection. The mock gate is regression coverage for packaging and orchestration; only the native gate satisfies the playable Phase 1 exit condition.

## Going further

- Install `@prismatix/runtime` when writing typed JavaScript or TypeScript extensions.
- See [Custom effects](custom-effects.md) for offline-compiled Stage shaders.
- Engine and runtime contributors should use the build instructions in the repository [README](../README.md#engine-contributors).
