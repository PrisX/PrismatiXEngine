function SplashScreen()
    Engine.PlaySFX("PrismatiXEngine_Logo.wav")
    Engine.FadeInBg("PrismatiXEngine_Logo.png", DisplayMode.Center, 450)
    Engine.Wait(500)
    Engine.FadeOutBg("PrismatiXEngine_Logo.png", DisplayMode.Center, 450)
end
