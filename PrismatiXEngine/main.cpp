#include <iostream>
#include <PrismatiXEngine.h>
int main(int argc, char* argv[]) {
	PrismatiXEngine engine;
	if (engine.Initialize("PrismatiX Visual Novel Engine", 1280, 720)) {
		engine.Run();
	}
	engine.Clean();
	return 0;
}