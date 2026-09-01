import {
  Button,
  Scene,
  Text,
  VBox,
  canonicalJson,
  compileJsxUi,
} from "@prismatix/authoring-sdk";

const authored = (
  <Scene name="JSX UI Example" width={1280} height={720}>
    <VBox name="Root" fill style={{background: "#16182AFF"}}>
      <Text name="Heading" bind={{text: "locale.ui.welcome"}}>
        Welcome
      </Text>
      <Button name="Start" size={[240, 64]} action="game.start">Start</Button>
    </VBox>
  </Scene>
);

const compiled = compileJsxUi(authored, {path: "Content/UI/Example.tsx"});
if (!compiled.valid || compiled.value === undefined)
  throw new Error(JSON.stringify(compiled.diagnostics));

// This is the only representation consumed by the native Runtime.
const canonicalUiJson = canonicalJson(compiled.value);
if (!canonicalUiJson.includes('"format": "PrismatiXUIScene"'))
  throw new Error("JSX did not lower to canonical PrismatiX UI JSON");

process.stdout.write("JSX UI authoring documentation example passed\n");
