import {Grid, Group, Scene, Text} from "@prismatix/authoring-sdk";
import {OverlayCloseButton, at} from "./Shared.tsx";

export default (
  <Scene name="Release Journey Save" width={1280} height={720}
    theme={[
      {id: "night", name: "Night", value: "#1C2B3AFA"},
      {id: "paperText", name: "Paper Text", value: "#F2F5F8FF"},
      {id: "gridSurface", name: "Grid Surface", value: "#0F1A24B8"},
      {id: "accent", name: "Accent", value: "#3F7CACFF"},
    ]}>
    <Group name="SaveRoot"
      appearance={{backgroundColor: "#1C2B3AFA", textColor: "#FFFFFFFF", opacity: 1, styleToken: "night"}}
      accessibilityLabel="存檔畫面" accessibilityRole="dialog" accessibilityFocusOrder={0}>
      <Text name="SaveTitle"
        layout={at(68, 46, 700, 58)} text="存檔  /  SAVE"
        appearance={{backgroundColor: "#00000000", textColor: "#F2F5F8FF", opacity: 1, styleToken: "paperText"}}
        fontSize={36} accessibilityLabel="存檔"
        accessibilityRole="heading" accessibilityFocusOrder={1} />
      <Grid name="Slots"
        layout={at(70, 142, 1140, 470, "fill")}
        appearance={{backgroundColor: "#0F1A24B8", textColor: "#FFFFFFFF", opacity: 1, styleToken: "gridSurface"}}
        runtimeProperties={{columns: 3, itemExtent: {type: "vec2", x: 350, y: 196},
          gap: {type: "vec2", x: 24, y: 22}, overscan: 1}}
        accessibilityLabel="存檔欄位" accessibilityRole="grid" accessibilityFocusOrder={10} />
      <OverlayCloseButton label="關閉存檔畫面" />
    </Group>
  </Scene>
);
