// Preloaded with --import: makes piped stdin/stderr look like a terminal, so
// tests can drive the CLI's prompts by writing key bytes to stdin.
Object.defineProperty(process.stdin, "isTTY", {value: true});
process.stdin.setRawMode = () => process.stdin;
Object.defineProperty(process.stderr, "isTTY", {value: true});
Object.defineProperty(process.stderr, "columns", {value: 100});
delete process.env.CI;
