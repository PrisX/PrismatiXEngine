import {Button, ColorRect, Group, Image, Scene, Text, actions} from "@prismatix/authoring-sdk";

// The title screen. Positions are fractions of the game resolution and sizes
// scale with it, so the layout holds at 1280x720, 1920x1080 or anything else.
const width = {{WIDTH}};
const height = {{HEIGHT}};
const title = "{{PROJECT_NAME}}";
const x = (fraction: number) => Math.round(fraction * width);
const y = (fraction: number) => Math.round(fraction * height);
const px = (value: number) => Math.round(value * Math.min(width / 1280, height / 720));

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
      <Image name="Backdrop" path="Assets/hill-dusk.jpg" fill scaleMode="Fill" />
      {/* Darkens the left third, where the title and menu sit. */}
      <ColorRect name="Scrim" fill color="#000000B3"
        style={{gradient: ["#000000B3", "#00000000", "horizontal"]}} />
      <Text name="Title" text={title} position={[x(0.075), y(0.33)]}
        size={[x(0.8), px(70)]} fontSize={px(48)}
        appearance={{textColor: "#FFFFFFF2"}} accessibilityRole="heading" />
      {menu.map((entry, index) =>
        <Button name={entry.name} text={entry.label} action={entry.action}
          position={[x(0.075) - px(8), y(0.33) + px(100 + index * 44)]}
          size={[px(220), px(36)]} fontSize={px(18)} horizontalAlignment="Left"
          style={{background: "#00000000", hoverBackground: "#FFFFFF14", radius: px(3),
            color: "#FFFFFFA6", hoverTextColor: "#FFFFFFFF"}}
          accessibilityFocusOrder={index + 1} />)}
      <Text name="Credit" text="Made with PrismatiX"
        position={[x(0.075), height - px(44)]} size={[px(400), px(20)]}
        fontSize={px(12)} appearance={{textColor: "#FFFFFF66"}} />
    </Group>
  </Scene>
);
