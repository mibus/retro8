#include "common.h"
#include "vm/gfx.h"

#include "io/loader.h"
#include "io/stegano.h"
#include "vm/machine.h"
#include "vm/input.h"

#include <cstring>
#include <stdint.h>
#include <future>
#include <SDL/SDL.h>

namespace r8 = retro8;
using pixel_t =
#ifdef RS90
uint8_t;
#else
uint16_t;
#endif

std::future<void> _initFuture;

uint32_t frameCounter = 0;
uint32_t lastFrameTick = 0;

constexpr int SAMPLE_RATE = 44100;

r8::Machine machine;
r8::io::Loader loader;

r8::input::InputManager input;
r8::gfx::ColorTable colorTable;
int16_t* audioBuffer;

#ifndef IPU_SCALING
SDL_Surface *real_screen;
#endif
SDL_Surface *sdl_screen;

struct ColorMapper
{
	r8::gfx::ColorTable::pixel_t operator()(uint8_t r, uint8_t g, uint8_t b) const
	{
		return SDL_MapRGB(sdl_screen->format, r, g, b);
	}
};


uint32_t Platform::getTicks() { return SDL_GetTicks(); }

void deinit()
{
	if (sdl_screen) SDL_FreeSurface(sdl_screen);
#ifndef IPU_SCALING
	if (real_screen) SDL_FreeSurface(real_screen);
#endif
	delete[] audioBuffer;
	//TODO: release all structures bound to Lua etc
}


void flip_screen()
{
	int fps = machine.code().require60fps() ? 60 : 30;
	auto* data = machine.memory().screenData();
	auto* screenPalette = machine.memory().paletteAt(r8::gfx::SCREEN_PALETTE_INDEX);
	auto output = static_cast<pixel_t*>(sdl_screen->pixels);

	for (size_t i = 0; i < r8::gfx::BYTES_PER_SCREEN; ++i)
	{
		const r8::gfx::color_byte_t* pixels = data + i;
		const auto rc1 = colorTable.get(screenPalette->get((pixels)->low()));
		const auto rc2 = colorTable.get(screenPalette->get((pixels)->high()));

		*(output) = rc1;
		*((output)+1) = rc2;
		(output) += 2;
	}

	#ifndef IPU_SCALING
	SDL_SoftStretch(sdl_screen, NULL, real_screen, NULL);
	SDL_Flip(real_screen);
	#else
	SDL_Flip(sdl_screen);
	#endif
	++frameCounter;

	/* Of course this assumes that the actual screen refresh rate is 60 or close to it.
	 * Most games are 30 FPS though. I'm not aware of any 60 fps games aside from one
	 * which only works on Picolove. - Gameblabla */
	#ifdef SDL_TRIPLEBUF
	if (fps != 60) {
	#endif
	//SDL_Delay(1000.0/fps);
	uint32_t now = SDL_GetTicks();
	if (now - lastFrameTick < 1000.0/fps)
		SDL_Delay(1000.0/fps - (now - lastFrameTick));
	lastFrameTick = SDL_GetTicks();
	//while((1000/fps) > SDL_GetTicks()-start)SDL_Delay((1000/fps)-(SDL_GetTicks()-start));
	#ifdef SDL_TRIPLEBUF
	}
	#endif
}


bool load_game(char* rom_name)
{
	size_t sz = 0;
	FILE* fp;
	uint8_t* bdata;

	input.reset();
	machine.setflip(flip_screen);
	machine.sound().init();
	frameCounter = 0;
	
	if (strstr(rom_name, ".PNG") || strstr(rom_name, ".png"))
	{
		fp = fopen(rom_name, "rb");
		if (fp)
		{
			fseek(fp, 0 , SEEK_END);
			sz = ftell(fp);
			fseek(fp, 0 , SEEK_SET);
			bdata = (uint8_t*)malloc(sz);
			fread(bdata, sz, 1, fp);
			fclose(fp);
		}
		else
		{
			return false;
		}
		
		std::vector<uint8_t> out;
		unsigned long width, height;
		auto result = Platform::loadPNG(out, width, height, (uint8_t*)bdata, sz, true);
		assert(result == 0);
		
		if (bdata) free(bdata);
		
		r8::io::Stegano stegano;
		stegano.load({ reinterpret_cast<const uint32_t*>(out.data()), nullptr, out.size() / 4 }, machine);
	}
	else
	{
		//TODO: not efficient since it's copied and it's not checking for '\0'
		std::string raw(rom_name);
		r8::io::Loader loader;
		loader.loadFile(raw, machine);
	}
		
	machine.memory().backupCartridge();

	if (machine.code().hasInit())
	{
		//_initFuture = std::async(std::launch::async, []() {
		printf("Cartridge has _init() function, calling it.\n");
		machine.code().init();
		printf("_init() function completed execution..\n");
		//});
	}
	return true;
}


void audio_callback(void* data, uint8_t* cbuffer, int length)
{
	retro8::sfx::APU* apu = static_cast<retro8::sfx::APU*>(data);
	int16_t* buffer = reinterpret_cast<int16_t*>(cbuffer);
	apu->renderSounds(buffer, length / sizeof(int16_t));
	return;
}


uint_fast8_t retro_run()
{
	Uint32 start;
	SDL_Event Event;

	/* manage input */
	while (SDL_PollEvent(&Event))
    {
		switch(Event.key.keysym.sym)
		{
			case SDLK_LEFT:
				input.manageKey(0, 0, Event.type == SDL_KEYDOWN);
			break;
			case SDLK_RIGHT:
				input.manageKey(0, 1, Event.type == SDL_KEYDOWN);
			break;
			case SDLK_UP:
				input.manageKey(0, 2, Event.type == SDL_KEYDOWN);
			break;
			case SDLK_DOWN:
				input.manageKey(0, 3, Event.type == SDL_KEYDOWN);
			break;

			case SDLK_z:
			case SDLK_LCTRL:
				input.manageKey(0, 4, Event.type == SDL_KEYDOWN);
			break;

			case SDLK_x:
			case SDLK_LALT:
				input.manageKey(0, 5, Event.type == SDL_KEYDOWN);
			break;

			case SDLK_a:
			case SDLK_SPACE:
				input.manageKey(1, 4, Event.type == SDL_KEYDOWN);
			break;

			case SDLK_s:
			case SDLK_LSHIFT:
				input.manageKey(1, 5, Event.type == SDL_KEYDOWN);
			break;
			case SDLK_ESCAPE:
				return 0;
			break;
			default:
			break;
		}
	}

	input.tick();
	
	flip_screen();
	input.manageKeyRepeat();

	return 1;
}

  
int main(int argc, char* argv[])
{
	int res = 0, while_res = 1;
	audioBuffer = new int16_t[SAMPLE_RATE * 2];
	
	SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO);
	#ifdef IPU_SCALING
	sdl_screen = SDL_SetVideoMode(128, 128, sizeof(pixel_t) * 8, SDL_HWSURFACE
	#else
	sdl_screen = SDL_CreateRGBSurface(SDL_HWSURFACE, 128, 128, sizeof(pixel_t) * 8, 0,0,0,0);
	real_screen = SDL_SetVideoMode(320, 240, sizeof(pixel_t) * 8, SDL_HWSURFACE
	#endif
	#ifdef SDL_TRIPLEBUF
	| SDL_TRIPLEBUF
	#else
	| SDL_DOUBLEBUF
	#endif
	 );
	SDL_ShowCursor(0);
	
	SDL_AudioSpec wantSpec, spec;
	wantSpec.freq = 44100;
	wantSpec.format = AUDIO_S16SYS;
	wantSpec.channels = 1;
	wantSpec.samples = 2048;
	wantSpec.userdata = &machine.sound();
	wantSpec.callback = audio_callback;

	SDL_OpenAudio(&wantSpec, &spec);
	SDL_PauseAudio(0);

	printf("Initializing audio buffer of %zu bytes\n", sizeof(int16_t) * SAMPLE_RATE * 2);

	colorTable.init(ColorMapper());
	machine.font().load();
	machine.code().loadAPI();
	input.setMachine(&machine);
	
	res = load_game(argv[1]);
	if (!res)
	{
		printf("Could not load game !\n");
		return 0;
	}
		
	while(while_res)
	{
		while_res = retro_run();
	}
	
	SDL_PauseAudio(1);
	deinit();
	SDL_Quit();
		
	return 0;
}

