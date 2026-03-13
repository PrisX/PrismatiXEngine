function SplashScreen()
    Engine.PlaySFX("PrismatiXEngine_Logo.wav")
    Engine.FadeInBg("PrismatiXEngine_Logo.png", DisplayMode.Center, 1000)
    Engine.Wait(1000)
    Engine.FadeOutBg("PrismatiXEngine_Logo.png", DisplayMode.Center, 1000)
end
