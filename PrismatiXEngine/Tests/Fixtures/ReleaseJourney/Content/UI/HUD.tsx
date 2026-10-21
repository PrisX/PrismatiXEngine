import {
  Button, Group, Scene, Text, VBox,
  type ButtonProps, type UiAppearance,
} from "@prismatix/authoring-sdk";
import {at} from "./Shared.tsx";

const transparent = (styleToken: string, textColor = "#FFFFFFFF"): UiAppearance =>
  ({backgroundColor: "#00000000", textColor, opacity: 1, styleToken});

interface QuickButtonProps {
  readonly name: string;
  readonly x: number;
  readonly text: string;
  readonly action: string;
  readonly label: string;
  readonly focusOrder: number;
}

function QuickButton(props: QuickButtonProps) {
  const style: ButtonProps["style"] = {
    background: "#2E4458FF", color: "#FFFFFFFF", opacity: 1, token: "toolbarButton",
    hoverBackground: "#3F7CACFF", focus: "#F2F5F8FF", disabledOpacity: 0.45,
  };
  return <Button name={props.name} position={[props.x, 646]} size={[74, 50]} align="center"
    style={style} action={props.action}
    focusMode="All" accessibilityLabel={props.label}
    accessibilityRole="button" accessibilityFocusOrder={props.focusOrder}>{props.text}</Button>;
}

export default (
  <Scene name="Release Journey HUD" width={1280} height={720}
    theme={[
      {id: "dialogueCard", name: "Dialogue Card", value: "#F2F5F8F2"},
      {id: "accent", name: "Accent", value: "#3F7CACFF"},
      {id: "transparent", name: "Transparent", value: "#00000000"},
      {id: "speaker", name: "Speaker Text", value: "#FFFFFFFF"},
      {id: "dialogueText", name: "Dialogue Text", value: "#1C2B3AFF"},
      {id: "choices", name: "Choices", value: "#00000000"},
      {id: "mode", name: "Mode Text", value: "#F2F5F8FF"},
      {id: "toolbarButton", name: "Toolbar Button", value: "#2E4458FF"},
    ]}>
    <Group name="HUDRoot"
      appearance={transparent("transparent")} accessibilityLabel="遊戲介面"
      accessibilityRole="window" accessibilityFocusOrder={0}>
      <Group name="DialogueCard"
        layout={at(46, 456, 1188, 180, "fill")}
        appearance={{backgroundColor: "#F2F5F8F2", textColor: "#1C2B3AFF", opacity: 1, styleToken: "dialogueCard"}} />
      <Group name="SpeakerTag"
        layout={at(72, 428, 250, 56)}
        appearance={{backgroundColor: "#3F7CACFF", textColor: "#FFFFFFFF", opacity: 1, styleToken: "accent"}} />
      <Text name="Speaker"
        layout={at(94, 440, 205, 36)} text="" appearance={transparent("speaker")}
        fontSize={25} accessibilityLabel="說話者"
        accessibilityRole="heading" accessibilityFocusOrder={2} />
      <Text name="Dialogue"
        layout={at(84, 496, 1096, 112)} text=""
        appearance={transparent("dialogueText", "#1C2B3AFF")}
        fontSize={29} wrap accessibilityLabel="對話文字"
        accessibilityRole="text" accessibilityFocusOrder={3} />
      <VBox name="Choices"
        layout={at(430, 220, 420, 190, "fill")} appearance={transparent("choices")}
        runtimeProperties={{separation: 10}} clipContent={false}
        accessibilityLabel="選項" accessibilityRole="listbox"
        accessibilityFocusOrder={10} />
      <Text name="ModeState"
        layout={at(930, 44, 145, 28, "end")} text="" appearance={transparent("mode", "#F2F5F8FF")}
        fontSize={16} accessibilityLabel="自動或快轉狀態"
        accessibilityRole="status" accessibilityFocusOrder={21} />
      <QuickButton name="BacklogQuick" x={760} text="記" action="backlog.open" label="開啟對話紀錄" focusOrder={23} />
      <QuickButton name="SaveQuick" x={842} text="存" action="save.open" label="開啟存檔" focusOrder={24} />
      <QuickButton name="LoadQuick" x={924} text="讀" action="load.open" label="開啟讀檔" focusOrder={25} />
      <QuickButton name="AutoQuick" x={1006} text="自" action="mode.auto" label="切換自動播放" focusOrder={26} />
      <QuickButton name="GalleryQuick" x={1088} text="憶" action="gallery.open" label="開啟回憶相簿" focusOrder={27} />
    </Group>
  </Scene>
);
