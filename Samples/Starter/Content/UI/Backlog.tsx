import {Button, Control, Group, Scene, Text} from "@prismatix/authoring-sdk";

// Ejected from the stock Player's built-in "backlog" screen (designed at
// 1280x720, scaled to this project). The Player finds controls by
// name (e.g. Dialogue, Slots) and fills the bound UI data, so keep those names
// and bindings when you restyle it; `prismatix validate` checks them.
const width = 1920;
const height = 1080;
const sx = (value: number) => value * width / 1280;
const sy = (value: number) => value * height / 720;
const sf = (value: number) => Math.sign(value) *
  Math.max(1, Math.round(Math.abs(value) * Math.min(width / 1280, height / 720)));

export default (
  <Scene name="PrismatiX Builtin Backlog" width={width} height={height}>
    <Group name="BacklogRoot"
      position={[sx(0), sy(0)]}
      size={[sx(1280), sy(720)]}
      layout={{"mode":"free","anchorX":0,"anchorY":0,"anchorRight":1,"anchorBottom":1,"pivotX":0,"pivotY":0,"margin":0,"alignment":"fill","sizeRule":"fill"}}
      appearance={{backgroundColor: "#000000D9"}}
      accessibilityLabel="Backlog"
      accessibilityRole="dialog"
      accessibilityFocusOrder={1}>
      <Text name="ScreenHeading"
        position={[sx(64), sy(40)]}
        size={[sx(800), sy(40)]}
        text="履歷"
        appearance={{textColor: "#FFFFFFEB"}}
        accessibilityLabel="履歷"
        accessibilityRole="heading"
        accessibilityFocusOrder={2}
        fontSize={sf(26)} />
      <Group name="HeadingRule"
        position={[sx(64), sy(92)]}
        size={[sx(1152), sy(1)]}
        appearance={{backgroundColor: "#FFFFFF24"}}
        accessibilityRole="group"
        accessibilityFocusOrder={3} />
      <Control name="Entries"
        kind="control"
        runtimeType="ListView"
        position={[sx(64), sy(116)]}
        size={[sx(1152), sy(528)]}
        accessibilityLabel="Backlog entries"
        accessibilityRole="list"
        accessibilityFocusOrder={4}
        runtimeProperties={{itemExtent: {type: "vec2", x: sx(1152), y: sy(56)}, gap: {type: "vec2", x: sx(0), y: sy(4)}}} />
      <Button name="Close"
        position={[sx(1136), sy(662)]}
        size={[sx(80), sy(34)]}
        text="返回"
        appearance={{textColor: "#FFFFFF99", hoverBackgroundColor: "#FFFFFF14", focusColor: "#FFFFFFCC", cornerRadius: sf(3), hoverTextColor: "#FFFFFFFF"}}
        action="overlay.close"
        accessibilityLabel="返回"
        accessibilityRole="button"
        accessibilityFocusOrder={5}
        focusMode="All"
        fontSize={sf(16)} />
    </Group>
  </Scene>
);
