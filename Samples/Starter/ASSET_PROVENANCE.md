# Starter asset provenance

## Font

`Content/Fonts/NotoSansTC-Bold.ttf` is Noto Sans Traditional Chinese Bold from the repository's `Resources/Fonts` inventory, under SIL Open Font License 1.1. Its SHA-256 is `ed4ae6ed781e34d0beee896e9be5d835a938ac7adabbcc1b6b6904290006f372`. Keep `Content/Fonts/OFL-1.1.txt` with distributions. Upstream: https://github.com/notofonts

## PrismatiX logo

`Content/UI/PrismatiX/PrismatiXEngine_Logo.png`, shown on black by the
startup screen `Content/UI/Splash.tsx`, is the PrismatiX Engine logo from the
repository's `Resources/Branding` (SHA-256
`4502d5660798694cc99e6306ed2625aa0f4ceca651a38f163a091236dbde0214`).
`Content/UI/PrismatiX/NOTICE.md` (kept by `create --empty`) says how it may be
used.

## Starter background provenance

`street.jpg` (in a new project: `Assets/Backgrounds/street.jpg`, written in a
Story as `[bg street]`) is the background PrismatiX Studio puts in every new
project: a sunny city street with a large tree, 1920x1080.

Source: supplied by the project owner (Owen Liu) as `sky.png`, a 1920x1080
RGB PNG (SHA-256
`8dbe020d52cd679298fca39bf2df87b400e241dcd75c44403c99434099e4e426`).

The PNG's embedded ComfyUI metadata records an image generation: checkpoint
`waiIllustriousSDXL_v160`, LoRA `GalgameBackground-Illustrious-r64-fro0.9`
at strength 1.0, prompt "scenery, no humans, city, road, day, blue sky, tree,
shadow", seed 330003410125970, 25 steps, euler sampler with the normal
scheduler, CFG 5, generated at 1920x1080. This is what the file says about
itself; it has not been checked further.

Shipped file: re-encoded as a JPEG (quality 90, no chroma subsampling) with the
metadata removed; SHA-256
`4c134ca7dda9cd5b2e386cf3e0574f9ba648aa21dece660cd78c65688ce90116`.

Licence: TBD. Pending the project owner's confirmation of redistribution rights
(including the terms of the model and LoRA named above). Until that is
confirmed, do not assume the picture may be redistributed: replace it before
releasing a game.
