import {Button, ColorRect, Group, Image, Scene, Text} from "@prismatix/authoring-sdk";

// The title screen. Positions are fractions of the game resolution and sizes
// scale with it, so the layout holds at 1280x720, 1920x1080 or anything else.
const width = {{WIDTH}};
const height = {{HEIGHT}};
const title = "{{PROJECT_NAME}}";
const x = (fraction: number) => Math.round(fraction * width);
const y = (fraction: number) => Math.round(fraction * height);
const px = (value: number) => Math.round(value * Math.min(width / 1280, height / 720));

const menu = [
  {name: "Start", label: "開始遊戲", action: "game.start"},
  {name: "Load", label: "讀取進度", action: "load.open"},
  {name: "Settings", label: "設定", action: "settings.open"},
  {name: "Quit", label: "結束", action: "app.quit"},
];

export default (
  <Scene name={`${title} Title`} width={width} height={height}>
    <Group name="TitleRoot" fill accessibilityRole="window" accessibilityLabel={title}>
      <Image name="Backdrop" path="Assets/window.png" fill scaleMode="Fill" />
      <ColorRect name="Scrim" fill color="#0E1117C8" />
      <Text name="Title" text={title} position={[x(0.075), y(0.24)]}
        size={[x(0.85), px(84)]} fontSize={px(60)}
        appearance={{textColor: "#F4F1EAFF"}} accessibilityRole="heading" />
      <ColorRect name="TitleRule" position={[x(0.077), y(0.24) + px(92)]}
        size={[px(56), Math.max(2, px(3))]} color="#E6C27AFF" />
      {menu.map((entry, index) =>
        <Button name={entry.name} text={entry.label} action={entry.action}
          position={[x(0.075), y(0.24) + px(128 + index * 58)]}
          size={[px(260), px(48)]} fontSize={px(22)}
          accessibilityFocusOrder={index + 1} />)}
      <Text name="Credit" text="Made with PrismatiX"
        position={[x(0.075), height - px(56)]} size={[px(400), px(24)]}
        fontSize={px(14)} appearance={{textColor: "#A3ABB9FF"}} />
    </Group>
  </Scene>
);
