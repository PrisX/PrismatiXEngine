import {Button, Group, Image, Scene, Text, type UiStyle} from "@prismatix/authoring-sdk";

const look = (styleToken: string, backgroundColor: string, textColor: string,
  opacity = 1): UiStyle =>
  ({token: styleToken, background: backgroundColor, color: textColor, opacity});

export default (
  <Scene name="Release Journey Title" width={1280} height={720}
    theme={[
      {id: "paper", name: "Paper", value: "#F2F5F8FF"},
      {id: "night", name: "Night", value: "#1C2B3AFF"},
      {id: "accent", name: "Accent", value: "#3F7CACFF"},
      {id: "ink", name: "Ink", value: "#1C2B3AFF"},
      {id: "art", name: "Art", value: "#FFFFFFFF"},
      {id: "quiet", name: "Quiet Button", value: "#DCE4ECFF"},
    ]}>
    <Group name="TitleRoot" style={look("paper", "#F2F5F8FF", "#1C2B3AFF")}
      accessibilityLabel="發行測試 標題畫面"
      accessibilityRole="window" accessibilityFocusOrder={0}>
      <Group name="NightBand" position={[0, 0]} size={[1280, 168]} align="fill"
        layout={{anchorRight: 1}} style={look("night", "#1C2B3AFF", "#FFFFFFFF")} />
      <Image name="KeyArt"
        assetId="30000000-0000-4000-8000-000000000001"
        position={[660, 120]} size={[560, 315]} align="center"
        style={look("art", "#00000000", "#FFFFFFFF")}
        scaleMode="Fit" lockAspectRatio
        accessibilityLabel="結局插圖" accessibilityRole="image"
        accessibilityFocusOrder={1} />
      <Text name="Title" position={[68, 186]} size={[585, 120]}
        text="發行測試" style={look("ink", "#00000000", "#1C2B3AFF")}
        fontSize={54} wrap accessibilityLabel="發行測試"
        accessibilityRole="heading" accessibilityFocusOrder={2} />
      <Button name="Start" position={[72, 488]} size={[330, 72]} align="center"
        style={{...look("accent", "#3F7CACFF", "#FFFFFFFF"),
          hoverBackground: "#5A95C4FF", focus: "#1C2B3AFF", disabledOpacity: 0.45}}
        action="game.start" focusMode="All"
        accessibilityLabel="開始遊戲" accessibilityRole="button"
        accessibilityFocusOrder={10}>開始  →</Button>
      <Button name="Gallery" position={[72, 576]} size={[230, 58]} align="center"
        style={{...look("quiet", "#DCE4ECFF", "#1C2B3AFF"),
          hoverBackground: "#E9EFF5FF", focus: "#3F7CACFF", disabledOpacity: 0.45}}
        action="gallery.open" focusMode="All"
        accessibilityLabel="打開回憶相簿" accessibilityRole="button"
        accessibilityFocusOrder={11}>回憶相簿</Button>
    </Group>
  </Scene>
);
