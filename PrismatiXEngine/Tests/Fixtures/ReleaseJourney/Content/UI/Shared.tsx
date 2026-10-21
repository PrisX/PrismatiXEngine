import {Button, type UiLayoutProps} from "@prismatix/authoring-sdk";

/** Fixed-size layout used by the HUD and the overlay screens. */
export const at = (x: number, y: number, width: number, height: number,
  alignment: UiLayoutProps["alignment"] = "start"): UiLayoutProps =>
  ({x, y, width, height, alignment, sizeRule: "fixed"});

interface OverlayCloseButtonProps {
  readonly label: string;
}

/** Shared authoring-only component imported by the overlay screens. */
export function OverlayCloseButton({label}: OverlayCloseButtonProps) {
  return (
    <Button name="Close" position={[1054, 636]} size={[156, 56]} align="center"
      style={{token: "accent", background: "#3F7CACFF", color: "#FFFFFFFF",
        hoverBackground: "#5A95C4FF", focus: "#F2F5F8FF", disabledOpacity: 0.45}}
      action="overlay.close" focusMode="All"
      accessibilityLabel={label} accessibilityRole="button" accessibilityFocusOrder={20}>
      返回  Esc
    </Button>
  );
}
