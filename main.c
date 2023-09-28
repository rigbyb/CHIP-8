#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>

const int WINDOW_WIDTH = 1280;
const int WINDOW_HEIGHT = 640;

const size_t MEMORY_SIZE = 0x1000;
const size_t PROGRAM_START = 0x200;
const size_t STACK_START = 0x52;
const size_t FONT_START = 0x50;
const int AUDIO_SAMPLE_RATE = 44100;
const float FRAME_TIME = 1000.0 / 60.0;

const uint8_t FONTS[16 * 5] = {
	0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
	0x20, 0x60, 0x20, 0x20, 0x70, // 1
	0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
	0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
	0x90, 0x90, 0xF0, 0x10, 0x10, // 4
	0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
	0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
	0xF0, 0x10, 0x20, 0x40, 0x40, // 7
	0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
	0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
	0xF0, 0x90, 0xF0, 0x90, 0x90, // A
	0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
	0xF0, 0x80, 0x80, 0x80, 0xF0, // C
	0xE0, 0x90, 0x90, 0x90, 0xE0, // D
	0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
	0xF0, 0x80, 0xF0, 0x80, 0x80  // F
};

struct State {
	uint8_t* memory;
	uint8_t regs_v[16];
	uint8_t delay_timer;
	uint8_t sound_timer;
	uint8_t keycode;
	uint16_t pc;
	uint16_t sp;
	uint16_t reg_i;
	bool end_of_program;
	bool waiting_for_key;
	uint64_t* video_buffer;
};

bool read_rom(const char* path, uint8_t** data, size_t* size) {
	FILE* file = NULL;
	
	if (fopen_s(&file, path, "rb") != 0) {
		fprintf(stderr, "Failed to open ROM file %s\n", path);
		return false;
	}

	fseek(file, 0, SEEK_END);
	*size = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (*size <= 0) {
		fprintf(stderr, "Failed to determine ROM size of %s\n", path);
		fclose(file);
		return false;
	}

	if (*size >= MEMORY_SIZE - PROGRAM_START) {
		fprintf(stderr, "Program %s is too big to load into memory\n", path);
		fclose(file);
		return false;
	}

	*data = malloc(*size);

	if (*data == NULL) {
		fprintf(stderr, "Failed to allocate buffer for ROM %s\n", path);
		fclose(file);
		return false;
	}

	size_t bytes_read = fread(*data, 1, *size, file);

	if (bytes_read != *size) {
		fprintf(stderr, "Couldn't read all contents of ROM %s\n", path);
		fclose(file);
		free(*data);
		return false;
	}

	fclose(file); 

	return true;
}

bool load_rom(struct State* state, const char* path) {
	uint8_t* data = NULL;
	size_t size = 0;
	
	if (read_rom(path, &data, &size) == false) {
		return false;
	}

	memcpy(&state->memory[PROGRAM_START], data, size);

	free(data);

	return true;
}

bool init_sdl(SDL_Window** window, SDL_Renderer** renderer, SDL_Texture** texture, SDL_AudioDeviceID* audio_device) {
	if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
		fprintf(stderr, "Failed to initialise SDL: %s\n", SDL_GetError());
		return false;
	}

	*window = SDL_CreateWindow(
		"CHIP-8",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,		// x and y
		WINDOW_WIDTH, WINDOW_HEIGHT,						// width and height
		0													// flags
	);

	if (*window == NULL) {
		fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
		return false;
	}

	*renderer = SDL_CreateRenderer(
		*window,
		-1,		// rendering driver (-1 = first supporting)
		0		// flags
	);

	if (*renderer == NULL) {
		fprintf(stderr, "Failed to create SDL renderer: %s\n", SDL_GetError());
		return false;
	}

	*texture = SDL_CreateTexture(
		*renderer,
		SDL_PIXELFORMAT_RGBA8888,
		SDL_TEXTUREACCESS_STREAMING,
		64,
		32
	);

	if (*texture == NULL) {
		fprintf(stderr, "Failed to create SDL texture: %s\n", SDL_GetError());
		return false;
	}

	SDL_AudioSpec audio_spec;
	SDL_zero(audio_spec);

	audio_spec.freq = AUDIO_SAMPLE_RATE;
	audio_spec.format = AUDIO_S16SYS;
	audio_spec.channels = 1;
	audio_spec.samples = 1024;
	audio_spec.callback = NULL;

	*audio_device = SDL_OpenAudioDevice(
		NULL,			// device name,
		0,				// opened for recording
		&audio_spec,	// desired spec
		NULL,			// obtained spec,
		0				// allowed changes flag
	);

	if (audio_device == 0) {
		fprintf(stderr, "Failed to open SDL audio device: %s\n", SDL_GetError());
		return false;
	}

	return true;
}

uint32_t* convert_video_to_sdl(uint64_t* video) {
	// 4 channels, even though it's only really greyscale
	uint32_t* result = calloc(64 * 32, sizeof(uint32_t));

	if (result == NULL) {
		fprintf(stderr, "Failed to allocate video buffer when converting.\n");
		return NULL;
	}
	
	for (int y = 0; y < 32; y++) {
		uint64_t row = video[y];

		for (int x = 0; x < 64; x++) {
			uint64_t mask = (1ULL << 63) >> x;

			if ((row & mask) == mask) {
				result[y * 64 + x] = 0xFFFFFFFF;
			}
		}
	}

	return result;
}

struct State* state_init() {
	struct State* state = malloc(sizeof(struct State));

	if (state == NULL) {
		fprintf(stderr, "Failed to allocate struct state.\n");
		return NULL;
	}

	// All 8 bit -> size of 1
	state->memory = calloc(MEMORY_SIZE, 1);

	if (state->memory == NULL) {
		fprintf(stderr, "Failed to allocate state memory buffer\n");
		return NULL;
	}

	memcpy(&state->memory[FONT_START], &FONTS, sizeof(FONTS));

	memset(state->regs_v, 0, sizeof(state->regs_v));

	state->delay_timer = 0;
	state->sound_timer = 0;
	// Signifies no key is being pressed
	state->keycode = 16;

	state->pc = PROGRAM_START;
	state->sp = STACK_START;
	state->reg_i = 0;

	// For fun, use malloc
	state->video_buffer = calloc(32, sizeof(uint64_t));

	state->end_of_program = false;
	state->waiting_for_key = false;

	return state;
}

void state_destroy(struct State* state) {
	free(state->memory);
	free(state->video_buffer);
	free(state);
}

void state_push_to_stack(struct State* state, uint16_t value) {
	// Flip endianness
	value = (value << 8) | (value >> 8);

	memcpy(&state->memory[state->sp], &value, sizeof(uint16_t));

	state->sp += 2;
}

uint16_t state_pop_from_stack(struct State* state) {
	state->sp -= 2;
	
	uint16_t address = state->memory[state->sp] << 8 | state->memory[state->sp + 1];

	memset(&state->memory[state->sp], 0, sizeof(uint16_t));

	return address;
}

void instruction_clear_video(struct State* state) {
	memset(state->video_buffer, 0, sizeof(uint64_t) * 32);
}

void instruction_draw_sprite(struct State* state, int regx, int regy, int height) {
	// Wraps starting position around
	uint8_t start_x = state->regs_v[regx] % 64;
	uint8_t start_y = state->regs_v[regy] % 32;

	state->regs_v[0xF] = 0;

	for (uint8_t row = 0; row < height; row++) {
		uint8_t y = start_y + row;

		if (y >= 32) {
			break;
		}

		// For each row, progress another byte
		uint64_t sprite_mask = (uint64_t)state->memory[state->reg_i + row];
		
		// Move sprite into place horizontally using bitshift
		// 55 = 64 - 1 - 8
		sprite_mask = (sprite_mask << 55) >> start_x;

		uint64_t row_data = state->video_buffer[y];

		state->video_buffer[y] = row_data ^ sprite_mask;
	
		// Set reg F if any flips occur
		if ((row_data & sprite_mask) != 0) {
			state->regs_v[0xF] = 1;
		}
	}
}

void instruction_decimal_digits(struct State* state, uint8_t value) {
	state->memory[state->reg_i] = value / 100;				// hundreds
	state->memory[state->reg_i + 1] = (value / 10) % 10;	// tens
	state->memory[state->reg_i + 2] = value % 10;			// ones
}

// TODO: Annonate instructions
void state_step(struct State* state) {
	// Reached end of program, or there are less than 2 bytes to read
	if (state->pc >= MEMORY_SIZE - 1) {
		state->end_of_program;
		return;
	}

	uint16_t opcode = state->memory[state->pc] << 8 | state->memory[state->pc + 1];

	uint8_t nibble1 = opcode >> 12;
	uint8_t nibble2 = (opcode & 0xF00) >> 8;
	uint8_t nibble3 = (opcode & 0xF0) >> 4;
	uint8_t nibble4 = opcode & 0xF;
	uint8_t nn = opcode & 0xFF;
	uint16_t nnn = opcode & 0xFFF;

	// printf("PC: 0x%04X OP: 0x%04X\n", state->pc, opcode);

	bool should_step = true;

	switch (nibble1) {
	case 0x0:
		switch (opcode) {
		case 0x00E0:
			instruction_clear_video(state);
			break;

		case 0x00EE:
			state->pc = state_pop_from_stack(state);
			should_step = false;
			break;

		default:
			printf("TODO: Call.\n");
			break;
		}

		break;

	case 0x1:
		state->pc = nnn;
		should_step = false;
		break;

	case 0x2:
		// Push the counter for the proceeding instruction
		state_push_to_stack(state, state->pc + 0x2);
		state->pc = nnn;
		should_step = false;

		break;

	case 0x3:
		if (state->regs_v[nibble2] == nn) {
			state->pc += 4;
			should_step = false;
		}

		break;

	case 0x4:
		if (state->regs_v[nibble2] != nn) {
			state->pc += 4;
			should_step = false;
		}

		break;

	case 0x5:
		if (state->regs_v[nibble2] == state->regs_v[nibble3]) {
			state->pc += 4;
			should_step = false;
		}

		break;

	case 0x6:
		state->regs_v[nibble2] = nn;
		break;

	case 0x7:
		state->regs_v[nibble2] += nn;
		break;

	case 0x8:
		switch (nibble4) {
		case 0x0:
			state->regs_v[nibble2] = state->regs_v[nibble3];
			break;

		case 0x1:
			state->regs_v[nibble2] |= state->regs_v[nibble3];
			break;

		case 0x2:
			state->regs_v[nibble2] &= state->regs_v[nibble3];
			break;

		case 0x3:
			state->regs_v[nibble2] ^= state->regs_v[nibble3];
			break;

		case 0x4:
			// Carry
			state->regs_v[0xF] = state->regs_v[nibble2] > state->regs_v[nibble2] + state->regs_v[nibble3];
			state->regs_v[nibble2] += state->regs_v[nibble3];

			break;

		case 0x5:
			// Carry
			state->regs_v[0xF] = state->regs_v[nibble2] < state->regs_v[nibble2] - state->regs_v[nibble3];
			state->regs_v[nibble2] -= state->regs_v[nibble3];

			break;

		case 0x6:
			state->regs_v[0xF] = state->regs_v[nibble2] & 0x1;
			state->regs_v[nibble2] >>= 1;
			break;

		case 0x7:
			// Carry
			// Carry
			state->regs_v[0xF] = state->regs_v[nibble3] < state->regs_v[nibble3] - state->regs_v[nibble2];
			state->regs_v[nibble2] = state->regs_v[nibble3] - state->regs_v[nibble2];

			break;

		case 0xE:
			state->regs_v[0xF] = state->regs_v[nibble2] >> 7;
			state->regs_v[nibble2] <<= 1;
			break;

		default: 
			printf("Unknown 0x8 ending 0x%04X\n", opcode);
			break;
		}
		break;

	case 0x9:
		if (state->regs_v[nibble2] != state->regs_v[nibble3]) {
			state->pc += 4;
			should_step = false;
		}

		break;

	case 0xA:
		state->reg_i = nnn;
		break;

	case 0xC:
		state->regs_v[nibble2] = rand() & nn;
		break;

	case 0xD:
		instruction_draw_sprite(state, nibble2, nibble3, nibble4);
		break;

	case 0xE:
		switch (nn) {
		case 0x9E:
			if (state->keycode == state->regs_v[nibble2]) {
				state->pc += 4;
				should_step = false;
			}

			break;

		case 0xA1:
			if (state->keycode != state->regs_v[nibble2]) {
				state->pc += 4;
				should_step = false;
			}

			break;

		default:
			printf("Unknown 0xE ending 0x%04X\n", opcode);
			break;
		}

		break;

	case 0xF:
		switch (nn) {
		case 0x07:
			state->regs_v[nibble2] = state->delay_timer;
			break;

		case 0x0A:
			// If a key is pressed
			if (state->keycode != 16) {
				state->regs_v[nibble2] = state->keycode;
			}
			else {
				// Stay still (block)
				should_step = false;
			}

			break;

		case 0x15:
			state->delay_timer = state->regs_v[nibble2];
			break;

		case 0x18:
			state->sound_timer = state->regs_v[nibble2];
			break;

		case 0x1E:
			state->reg_i += state->regs_v[nibble2];
			break;

		case 0x29:
			state->reg_i = FONT_START + state->regs_v[nibble2];
			break;

		case 0x33:
			instruction_decimal_digits(state, state->regs_v[nibble2]);
			break;

		case 0x55:
			memcpy(&state->memory[state->reg_i], state->regs_v, nibble2 + 1);
			break;

		case 0x65:
			memcpy(state->regs_v, &state->memory[state->reg_i], nibble2 + 1);
			break;

		default: 
			printf("Unknown 0xF ending 0x%04X\n", opcode);
			break;
		}

		break;

	default:
		printf("Unknown opcode: 0x%04X\n", opcode);
		break;
	}

	if (should_step == true) {
		state->pc += 2;
	}
}

// TODO: Cleanup
int get_chip8_keycode_from_sdl(SDL_Keycode keycode) {
	switch (keycode) {
	case SDLK_1: return 0x1;
	case SDLK_2: return 0x2;
	case SDLK_3: return 0x3;
	case SDLK_4: return 0xC;

	case SDLK_q: return 0x4;
	case SDLK_w: return 0x5;
	case SDLK_e: return 0x6;
	case SDLK_r: return 0xD;

	case SDLK_a: return 0x7;
	case SDLK_s: return 0x8;
	case SDLK_d: return 0x9;
	case SDLK_f: return 0xE;

	case SDLK_z: return 0xA;
	case SDLK_x: return 0x0;
	case SDLK_c: return 0xB;
	case SDLK_v: return 0xF;

	default:
		// Signifies no key is being pressed
		return 16;
	}
}

int main(int argc, char* argv[]) {
	if (argc < 2) {
		fprintf(stderr, "No ROM file provided.\n");
		return 1;
	}

	const char* rom_path = argv[1];
		
	SDL_Window* window = NULL;
	SDL_Renderer* renderer = NULL;
	SDL_Texture* video_texture = NULL;
	SDL_AudioDeviceID audio_device = 0;

	if (init_sdl(&window, &renderer, &video_texture, &audio_device) == false) {
		return 1;
	}

	SDL_PauseAudioDevice(audio_device, 0);

	struct State* state = state_init();

	if (state == NULL) {
		return 1;
	}

	if (load_rom(state, rom_path) == false) {
		return 1;
	}

	uint32_t last_time = SDL_GetTicks();

	bool is_running = true;

	while (is_running) {
		SDL_Event event;

		while (SDL_PollEvent(&event)) {
			switch (event.type) {
			case SDL_KEYDOWN:
				state->keycode = get_chip8_keycode_from_sdl(event.key.keysym.sym);
				break;

			case SDL_KEYUP:
				// Signifies no key is being pressed
				state->keycode = 16;
				break;

			case SDL_QUIT:
				is_running = false;
				break;
			
			default: break;
			}
		}

		// May have changed after processing events.
		if (is_running == false) {
			break;
		}

		// Emulation
		uint32_t current_time = SDL_GetTicks();
		uint32_t elapsed_time = current_time - last_time;

		if (elapsed_time >= FRAME_TIME) {
			if (state->delay_timer > 0) {
				state->delay_timer -= 1;
			}

			if (state->sound_timer > 0) {
				state->sound_timer -= 1;
			}

			last_time = current_time;
		}

		//printf("%i\n", state->sound_timer);

		state_step(state);

		if (state->end_of_program) {
			is_running = false;
			break;
		}

		// Sound
		if (state->sound_timer > 0) {
			if (elapsed_time > 0) {
				for (int i = 0; i < elapsed_time; i++) {
					int16_t sample = sin(i * 0.05) * 5000;

					SDL_QueueAudio(audio_device, &sample, sizeof(int16_t));
				}
			}
		}

		// Rendering
		uint32_t* video_buffer_sdl = convert_video_to_sdl(state->video_buffer);

		if (video_buffer_sdl) {
			void* pixels;
			int pitch;
			SDL_LockTexture(video_texture, NULL, &pixels, &pitch);

			memcpy(pixels, video_buffer_sdl, 64 * 32 * sizeof(uint32_t));

			SDL_UnlockTexture(video_texture);

			SDL_Rect texture_rect;
			texture_rect.x = 0;
			texture_rect.y = 0;
			texture_rect.w = WINDOW_WIDTH;
			texture_rect.h = WINDOW_HEIGHT;

			SDL_RenderCopy(renderer, video_texture, NULL, &texture_rect);

			free(video_buffer_sdl);
		}

		SDL_RenderPresent(renderer);
	}

	state_destroy(state);

	SDL_CloseAudioDevice(audio_device);
	SDL_DestroyTexture(video_texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}