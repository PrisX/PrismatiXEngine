# Starter asset provenance

The starter's pictures were generated for PrismatiX with Anima Base 1.0
(CircleStone Labs, https://huggingface.co/circlestone-labs/Anima) in ComfyUI.
The Anima license restricts the model weights to non-commercial use but places
no restriction on generated images, which may be used commercially. The
pictures are distributed with the PrismatiX repository; keep them in your game
or replace them.

Common settings: Qwen3 0.6B text encoder, Qwen-Image VAE, er_sde sampler with
the simple scheduler, 30 steps, CFG 4, negative prompt "worst quality, low
quality, score_1, score_2, score_3, artist name, signature, watermark, blurry,
jpeg artifacts, chromatic aberration".

## Character

`Assets/hikari-smile.png` is a 1024x1536 generation (seed 33) with the
anima-highres-aesthetic-boost LoRA at 0.7. The prompt mixes the artist tags
@tiv, @kobuichi and @karory, so no single artist's style is copied, and
describes a full-body standing high school girl (light brown medium hair, hair
ribbon, beige cardigan, navy pleated skirt) on a white background.

`Assets/hikari-happy.png`, `Assets/hikari-talk.png` and
`Assets/hikari-surprised.png` repaint only the face of that image (an elliptical
mask, denoise 0.8, seed 1) with expression tags; every pixel outside the face is
identical, so expressions swap without the body shifting. All four share one
BiRefNet_toonout matte, were cleared of the white fringe and scaled to 700
pixels high.

## Backgrounds

All three use the artist tag @y y (ysk ygc), were generated at 1536x864 and
scaled to 1920x1080.

- `Assets/room-dusk.jpg`: a girl's bedroom at sunset, seed 5151.
- `Assets/room-night.jpg`: the dusk room tinted dark blue, then repainted at
  denoise 0.55 with a night prompt, so the layout matches.
- `Assets/hill-dusk.jpg`: a hilltop over a town at dusk, seed 303, for the
  title screen.

## Font

`Content/Fonts/NotoSansTC-Bold.ttf` is Noto Sans Traditional Chinese Bold from the repository's `Resources/Fonts` inventory, under SIL Open Font License 1.1. Its SHA-256 is `ed4ae6ed781e34d0beee896e9be5d835a938ac7adabbcc1b6b6904290006f372`. Keep `Content/Fonts/OFL-1.1.txt` with distributions. Upstream: https://github.com/notofonts

## PrismatiX logo

`Content/UI/PrismatiX/PrismatiXEngine_Logo.png`, shown on black by the
startup screen `Content/UI/Splash.tsx`, is the PrismatiX Engine logo from the
repository's `Resources/Branding` (SHA-256
`4502d5660798694cc99e6306ed2625aa0f4ceca651a38f163a091236dbde0214`).
`Content/UI/PrismatiX/NOTICE.md` (kept by `create --empty`) says how it may be
used.
