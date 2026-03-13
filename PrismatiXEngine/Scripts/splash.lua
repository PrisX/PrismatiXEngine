function SplashScreen()
    Engine.PlaySFX("murasame_yuzu.ogg")
    Engine.FadeInBg("yuzusoft.png", DisplayMode.Center, 1000, 255, 255, 255, 255)
    Engine.Wait(1000)
    Engine.FadeOutBg("yuzusoft.png", DisplayMode.Center, 1000, 255, 255, 255, 255)
end
