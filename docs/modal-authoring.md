# Modal dialogs in TSX UI

`<Modal>` creates a full-screen backdrop and a blocking panel while native UI
input keeps focus inside the visible dialog. Its children use coordinates
relative to `panel`; the backdrop may use an image or the default translucent
color. `backdropAction` is optional: without it, clicks outside the panel are
still consumed, but do not dismiss the dialog.

```tsx
import {Button, Modal, Scene, Text} from "@prismatix/authoring-sdk";

export default <Scene name="Settings" width={1280} height={720}>
  <Modal name="Confirm" fill visible={false}
    bind={{visible: "confirm.open"}}
    panel={{x: 390, y: 240, width: 500, height: 240}}
    backdropAction="confirm.cancel">
    <Text name="Prompt" x={20} y={24} width={460} height={60}>
      Save these settings?
    </Text>
    <Button name="Yes" x={20} y={170} width={120} height={40}
      action="confirm.accept">Yes</Button>
  </Modal>
</Scene>;
```

When `visible` becomes true, pointer events outside the dialog cannot reach
the underlying screen, and keyboard/gamepad focus moves to the first focusable
child. Hiding the dialog restores the previous focus if that control still
exists. The last visible modal in render order owns capture; a nested modal
can temporarily supersede its parent. The panel itself blocks backdrop clicks
within its rectangle. This is an in-scene dialog; route-level modal navigation
uses the separate UI router API.
