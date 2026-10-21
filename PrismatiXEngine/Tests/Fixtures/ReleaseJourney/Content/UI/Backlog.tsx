import {Control, Group, Scene, Text} from "@prismatix/authoring-sdk";
import {OverlayCloseButton, at} from "./Shared.tsx";

export default (
  <Scene name="Release Journey Backlog" width={1280} height={720}
    theme={[
      {id: "night", name: "Night", value: "#1C2B3AFA"},
      {id: "paperText", name: "Paper Text", value: "#F2F5F8FF"},
      {id: "listSurface", name: "List Surface", value: "#0F1A24B8"},
      {id: "accent", name: "Accent", value: "#3F7CACFF"},
    ]}>
    <Group name="BacklogRoot"
      appearance={{backgroundColor: "#1C2B3AFA", textColor: "#FFFFFFFF", opacity: 1, styleToken: "night"}}
      accessibilityLabel="對話紀錄" accessibilityRole="dialog" accessibilityFocusOrder={0}>
      <Text name="BacklogTitle"
        layout={at(68, 46, 700, 58)} text="對話紀錄  /  BACKLOG"
        appearance={{backgroundColor: "#00000000", textColor: "#F2F5F8FF", opacity: 1, styleToken: "paperText"}}
        fontSize={36} accessibilityLabel="對話紀錄"
        accessibilityRole="heading" accessibilityFocusOrder={1} />
      <Control runtimeType="ListView" name="Entries"
        layout={at(70, 164, 1140, 450, "fill")}
        appearance={{backgroundColor: "#0F1A24B8", textColor: "#FFFFFFFF", opacity: 1, styleToken: "listSurface"}}
        runtimeProperties={{columns: 1, itemExtent: {type: "vec2", x: 1100, y: 64},
          gap: {type: "vec2", x: 0, y: 10}, overscan: 2}}
        accessibilityLabel="已讀對話列表" accessibilityRole="log" accessibilityFocusOrder={10} />
      <OverlayCloseButton label="關閉對話紀錄" />
    </Group>
  </Scene>
);
