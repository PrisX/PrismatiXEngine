import {Button, ColorRect, Group, Image, Scene, Text, actions} from "@prismatix/authoring-sdk";

// The title screen. Positions are fractions of the game resolution and sizes
// scale with it, so the layout holds at 1280x720, 1920x1080 or anything else.
const width = {{WIDTH}};
const height = {{HEIGHT}};
const title = "{{PROJECT_NAME}}";
const x = (fraction: number) => Math.round(fraction * width);
const y = (fraction: number) => Math.round(fraction * height);
const px = (value: number) => Math.round(value * Math.min(width / 1280, height / 720));

// One palette for the whole screen: ink glass, warm text, a champagne accent.
const ink = "#0B0F1E";
const accent = "#E9CF94FF";

const menu = [
  {name: "Start", label: "開始遊戲", action: actions.game.start},
  {name: "Continue", label: "繼續遊戲", action: actions.game.continue},
  {name: "Load", label: "讀取進度", action: actions.screen.load},
  {name: "Settings", label: "設定", action: actions.screen.settings},
  {name: "Quit", label: "結束", action: actions.app.quit},
];

export default (
  <Scene name={`${title} Title`} width={width} height={height}>
    <Group name="TitleRoot" fill accessibilityRole="window" accessibilityLabel={title}>
      <Image name="Backdrop" path="Assets/window.png" fill scaleMode="Fill" />
      {/* A scrim that darkens only the left, where the menu sits. */}
      <ColorRect name="Scrim" fill color={`${ink}E6`}
        style={{gradient: [`${ink}E6`, `${ink}00`, "horizontal"]}} />
      <Text name="Eyebrow" text="A VISUAL NOVEL" position={[x(0.085), y(0.25)]}
        size={[x(0.5), px(22)]} fontSize={px(14)} letterSpacing={px(6)}
        appearance={{textColor: accent}} />
      <Text name="Title" text={title} position={[x(0.082), y(0.25) + px(24)]}
        size={[x(0.8), px(90)]} fontSize={px(64)}
        appearance={{textColor: "#F7F3EBFF"}} accessibilityRole="heading" />
      <ColorRect name="TitleRule" position={[x(0.086), y(0.25) + px(120)]}
        size={[px(60), Math.max(2, px(3))]} color={accent} style={{radius: px(2)}} />
      {menu.map((entry, index) =>
        <Button name={entry.name} text={entry.label} action={entry.action}
          position={[x(0.085), y(0.25) + px(152 + index * 54)]}
          size={[px(272), px(42)]} fontSize={px(19)}
          style={{background: "#FFFFFF10", hoverBackground: "#E9CF942E",
            color: "#EEF1F7FF", hoverTextColor: "#F6E6BEFF", radius: px(21),
            border: [1, "#FFFFFF33"], hoverBorderColor: accent}}
          accessibilityFocusOrder={index + 1} />)}
      <Text name="Credit" text="Made with PrismatiX"
        position={[x(0.085), height - px(52)]} size={[px(400), px(22)]}
        fontSize={px(13)} appearance={{textColor: "#A3ABB9FF"}} />
    </Group>
  </Scene>
);
