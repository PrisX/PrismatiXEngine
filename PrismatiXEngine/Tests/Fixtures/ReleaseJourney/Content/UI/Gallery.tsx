import {Grid, Group, Scene, Text} from "@prismatix/authoring-sdk";
import {OverlayCloseButton, at} from "./Shared.tsx";

export default (
  <Scene name="Release Journey Gallery" width={1280} height={720}
    theme={[
      {id: "night", name: "Night", value: "#1C2B3AFF"},
      {id: "accent", name: "Accent", value: "#3F7CACFF"},
      {id: "paperText", name: "Paper Text", value: "#F2F5F8FF"},
      {id: "galleryGrid", name: "Gallery Grid", value: "#00000000"},
    ]}>
    <Group name="GalleryRoot"
      appearance={{backgroundColor: "#1C2B3AFF", textColor: "#FFFFFFFF", opacity: 1, styleToken: "night"}}
      accessibilityLabel="回憶相簿" accessibilityRole="dialog" accessibilityFocusOrder={0}>
      <Text name="GalleryTitle"
        layout={at(68, 48, 700, 62)} text="回憶相簿  /  GALLERY"
        appearance={{backgroundColor: "#00000000", textColor: "#F2F5F8FF", opacity: 1, styleToken: "paperText"}}
        fontSize={38} accessibilityLabel="回憶相簿"
        accessibilityRole="heading" accessibilityFocusOrder={1} />
      <Grid name="Items"
        layout={at(70, 180, 1000, 450, "fill")}
        appearance={{backgroundColor: "#00000000", textColor: "#FFFFFFFF", opacity: 1, styleToken: "galleryGrid"}}
        runtimeProperties={{columns: 2, itemExtent: {type: "vec2", x: 450, y: 300},
          gap: {type: "vec2", x: 20, y: 20}, overscan: 1}}
        accessibilityLabel="已解鎖回憶" accessibilityRole="grid" accessibilityFocusOrder={10} />
      <OverlayCloseButton label="關閉回憶相簿" />
    </Group>
  </Scene>
);
