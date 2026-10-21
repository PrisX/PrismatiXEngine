import {Grid, Group, Scene, Text} from "@prismatix/authoring-sdk";
import {OverlayCloseButton, at} from "./Shared.tsx";

export default (
  <Scene name="Release Journey Load" width={1280} height={720}
    theme={[
      {id: "night", name: "Night", value: "#1C2B3AFA"},
      {id: "paperText", name: "Paper Text", value: "#F2F5F8FF"},
      {id: "gridSurface", name: "Grid Surface", value: "#0F1A24B8"},
      {id: "accent", name: "Accent", value: "#3F7CACFF"},
    ]}>
    <Group name="LoadRoot"
      appearance={{backgroundColor: "#1C2B3AFA", textColor: "#FFFFFFFF", opacity: 1, styleToken: "night"}}
      accessibilityLabel="讀檔畫面" accessibilityRole="dialog" accessibilityFocusOrder={0}>
      <Text name="LoadTitle"
        layout={at(68, 46, 700, 58)} text="讀檔  /  LOAD"
        appearance={{backgroundColor: "#00000000", textColor: "#F2F5F8FF", opacity: 1, styleToken: "paperText"}}
        fontSize={36} accessibilityLabel="讀檔"
        accessibilityRole="heading" accessibilityFocusOrder={1} />
      <Grid name="Slots"
        layout={at(70, 142, 1140, 470, "fill")}
        appearance={{backgroundColor: "#0F1A24B8", textColor: "#FFFFFFFF", opacity: 1, styleToken: "gridSurface"}}
        runtimeProperties={{columns: 3, itemExtent: {type: "vec2", x: 350, y: 196},
          gap: {type: "vec2", x: 24, y: 22}, overscan: 1}}
        accessibilityLabel="讀檔欄位" accessibilityRole="grid" accessibilityFocusOrder={10} />
      <OverlayCloseButton label="關閉讀檔畫面" />
    </Group>
  </Scene>
);
