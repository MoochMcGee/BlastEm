#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "romdb.h"
#include "util.h"
#include "blastem.h"

#define TITLE_START 0x150
#define TITLE_END (TITLE_START+48)
#define GAME_ID_OFF 0x183
#define GAME_ID_LEN 8
#define ROM_END   0x1A4
#define RAM_ID    0x1B0
#define RAM_FLAGS 0x1B2
#define RAM_START 0x1B4
#define RAM_END   0x1B8
#define REGION_START 0x1F0

enum {
	I2C_IDLE,
	I2C_START,
	I2C_DEVICE_ACK,
	I2C_ADDRESS_HI,
	I2C_ADDRESS_HI_ACK,
	I2C_ADDRESS,
	I2C_ADDRESS_ACK,
	I2C_READ,
	I2C_READ_ACK,
	I2C_WRITE,
	I2C_WRITE_ACK
};

char * i2c_states[] = {
	"idle",
	"start",
	"device ack",
	"address hi",
	"address hi ack",
	"address",
	"address ack",
	"read",
	"read_ack",
	"write",
	"write_ack"
};

void eeprom_init(eeprom_state *state, uint8_t *buffer, uint32_t size)
{
	state->slave_sda = 1;
	state->host_sda = state->scl = 0;
	state->buffer = buffer;
	state->size = size;
	state->state = I2C_IDLE;
}

void set_host_sda(eeprom_state *state, uint8_t val)
{
	if (state->scl) {
		if (val & ~state->host_sda) {
			//low to high, stop condition
			state->state = I2C_IDLE;
			state->slave_sda = 1;
		} else if (~val & state->host_sda) {
			//high to low, start condition
			state->state = I2C_START;
			state->slave_sda = 1;
			state->counter = 8;
		}
	}
	state->host_sda = val;
}

void set_scl(eeprom_state *state, uint8_t val)
{
	if (val & ~state->scl) {
		//low to high transition
		switch (state->state)
		{
		case I2C_START:
		case I2C_ADDRESS_HI:
		case I2C_ADDRESS:
		case I2C_WRITE:
			state->latch = state->host_sda << 7 | state->latch >> 1;
			state->counter--;
			if (!state->counter) {
				switch (state->state & 0x7F)
				{
				case I2C_START:
					state->state = I2C_DEVICE_ACK;
					break;
				case I2C_ADDRESS_HI:
					state->address = state->latch << 8;
					state->state = I2C_ADDRESS_HI_ACK;
					break;
				case I2C_ADDRESS:
					state->address |= state->latch;
					state->state = I2C_ADDRESS_ACK;
					break;
				case I2C_WRITE:
					state->buffer[state->address] = state->latch;
					state->state = I2C_WRITE_ACK;
					state->address++;
					//TODO: page mask
					state->address &= state->size-1;
					break;
				}
			}
			break;
		case I2C_DEVICE_ACK:
			if (state->latch & 1) {
				state->state = I2C_READ;
				state->counter = 8;
				state->latch = state->buffer[state->address];
				state->address++;
				//TODO: page mask
				state->address &= state->size-1;
			} else {
				if (state->size < 256) {
					state->address = state->latch >> 1;
					state->state = I2C_WRITE;
				} else if (state->size < 8192) {
					state->address = state->latch << 8;
					state->state = I2C_ADDRESS;
				} else {
					state->state = I2C_ADDRESS_HI;
				}
				state->counter = 8;
			}
			break;
		case I2C_ADDRESS_HI_ACK:
			state->state = I2C_ADDRESS;
			break;
		case I2C_ADDRESS_ACK:
			state->state = I2C_WRITE;
			break;
		case I2C_READ:
			state->counter--;
			if (!state->counter) {
				state->state = I2C_READ_ACK;
			}
			break;
		case I2C_READ_ACK:
			state->state = I2C_READ;
			break;
		case I2C_WRITE_ACK:
			state->state = I2C_WRITE;
			break;
		}
	} else if (~val & state->scl) {
		//high to low transition
		switch (state->state & 0x7F)
		{
		case I2C_DEVICE_ACK:
		case I2C_ADDRESS_HI_ACK:
		case I2C_ADDRESS_ACK:
		case I2C_READ_ACK:
		case I2C_WRITE_ACK:
			state->slave_sda = 0;
			break;
		case I2C_READ:
			state->slave_sda = state->latch >> 7;
			state->latch = state->latch << 1;
			break;
		default:
			state->slave_sda = 1;
			break;
		}
	}
	state->scl = val;
}

uint8_t get_sda(eeprom_state *state)
{
	return state->host_sda & state->slave_sda;
}

uint16_t read_sram_w(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_type)
	{
	case RAM_FLAG_BOTH:
		return gen->save_storage[address] << 8 | gen->save_storage[address+1];
	case RAM_FLAG_EVEN:
		return gen->save_storage[address >> 1] << 8 | 0xFF;
	case RAM_FLAG_ODD:
		return gen->save_storage[address >> 1] | 0xFF00;
	}
	return 0xFFFF;//We should never get here
}

uint8_t read_sram_b(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_type)
	{
	case RAM_FLAG_BOTH:
		return gen->save_storage[address];
	case RAM_FLAG_EVEN:
		if (address & 1) {
			return 0xFF;
		} else {
			return gen->save_storage[address >> 1];
		}
	case RAM_FLAG_ODD:
		if (address & 1) {
			return gen->save_storage[address >> 1];
		} else {
			return 0xFF;
		}
	}
	return 0xFF;//We should never get here
}

m68k_context * write_sram_area_w(uint32_t address, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	if ((gen->bank_regs[0] & 0x3) == 1) {
		address &= gen->save_ram_mask;
		switch(gen->save_type)
		{
		case RAM_FLAG_BOTH:
			gen->save_storage[address] = value >> 8;
			gen->save_storage[address+1] = value;
			break;
		case RAM_FLAG_EVEN:
			gen->save_storage[address >> 1] = value >> 8;
			break;
		case RAM_FLAG_ODD:
			gen->save_storage[address >> 1] = value;
			break;
		}
	}
	return context;
}

m68k_context * write_sram_area_b(uint32_t address, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	if ((gen->bank_regs[0] & 0x3) == 1) {
		address &= gen->save_ram_mask;
		switch(gen->save_type)
		{
		case RAM_FLAG_BOTH:
			gen->save_storage[address] = value;
			break;
		case RAM_FLAG_EVEN:
			if (!(address & 1)) {
				gen->save_storage[address >> 1] = value;
			}
			break;
		case RAM_FLAG_ODD:
			if (address & 1) {
				gen->save_storage[address >> 1] = value;
			}
			break;
		}
	}
	return context;
}

m68k_context * write_bank_reg_w(uint32_t address, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	address &= 0xE;
	address >>= 1;
	gen->bank_regs[address] = value;
	if (!address) {
		if (value & 1) {
			context->mem_pointers[2] = NULL;
		} else {
			context->mem_pointers[2] = cart + 0x200000/2;
		}
	}
	return context;
}

m68k_context * write_bank_reg_b(uint32_t address, m68k_context * context, uint8_t value)
{
	if (address & 1) {
		genesis_context * gen = context->system;
		address &= 0xE;
		address >>= 1;
		gen->bank_regs[address] = value;
		if (!address) {
			if (value & 1) {
				context->mem_pointers[2] = NULL;
			} else {
				context->mem_pointers[2] = cart + 0x200000/2;
			}
		}
	}
	return context;
}
eeprom_map *find_eeprom_map(uint32_t address, genesis_context *gen)
{
	for (int i = 0; i < gen->num_eeprom; i++)
	{
		if (address >= gen->eeprom_map[i].start && address <= gen->eeprom_map[i].end) {
			return  gen->eeprom_map + i;
		}
	}
	return NULL;
}

void * write_eeprom_i2c_w(uint32_t address, void * context, uint16_t value)
{
	genesis_context *gen = ((m68k_context *)context)->system;
	eeprom_map *map = find_eeprom_map(address, gen);
	if (!map) {
		fprintf(stderr, "Could not find EEPROM map for address %X\n", address);
		exit(1);
	}
	printf("EEPROM word write: %X - %X\n", address, value);
	if (map->sda_write_mask) {
		printf("sda: %d\n", (value & map->sda_write_mask) != 0);
		set_host_sda(&gen->eeprom, (value & map->sda_write_mask) != 0);
	}
	if (map->scl_mask) {
		set_scl(&gen->eeprom, (value & map->scl_mask) != 0);
		printf("scl: %d, state: %s, counter: %d\n", (value & map->scl_mask) != 0, i2c_states[gen->eeprom.state], gen->eeprom.counter);
	}
	return context;
}

void * write_eeprom_i2c_b(uint32_t address, void * context, uint8_t value)
{
	genesis_context *gen = ((m68k_context *)context)->system;
	eeprom_map *map = find_eeprom_map(address, gen);
	if (!map) {
		fprintf(stderr, "Could not find EEPROM map for address %X\n", address);
		exit(1);
	}
	
	uint16_t expanded, mask;
	if (address & 1) {
		expanded = value;
		mask = 0xFF;
	} else {
		expanded = value << 8;
		mask = 0xFF00;
	}
	printf("EEPROM byte write: %X - %X (using mask %X and expanded val %X)\n", address, value, mask, expanded);
	if (map->sda_write_mask & mask) {
		printf("sda: %d\n", (expanded & map->sda_write_mask) != 0);
		set_host_sda(&gen->eeprom, (expanded & map->sda_write_mask) != 0);
	}
	if (map->scl_mask & mask) {
		printf("scl: %d, state: %s\n", (expanded & map->scl_mask) != 0, i2c_states[gen->eeprom.state]);
		set_scl(&gen->eeprom, (expanded & map->scl_mask) != 0);
	}
	return context;
}

uint16_t read_eeprom_i2c_w(uint32_t address, void * context)
{
	genesis_context *gen = ((m68k_context *)context)->system;
	eeprom_map *map = find_eeprom_map(address, gen);
	if (!map) {
		fprintf(stderr, "Could not find EEPROM map for address %X\n", address);
		exit(1);
	}
	uint16_t ret = 0;
	if (map->sda_read_bit < 16) {
		ret = get_sda(&gen->eeprom) << map->sda_read_bit;
	}
	printf("EEPROM word read: %X - %X\n", address, ret);
	return ret;
}

uint8_t read_eeprom_i2c_b(uint32_t address, void * context)
{
	genesis_context *gen = ((m68k_context *)context)->system;
	eeprom_map *map = find_eeprom_map(address, gen);
	if (!map) {
		fprintf(stderr, "Could not find EEPROM map for address %X\n", address);
		exit(1);
	}
	uint8_t bit = address & 1 ? map->sda_read_bit : map->sda_read_bit - 8;
	uint8_t ret = 0;
	if (bit < 8) {
		ret = get_sda(&gen->eeprom) << bit;
	}
	printf("EEPROM byte read: %X - %X\n", address, ret);
	return ret;
}

tern_node *load_rom_db()
{
	char *exe_dir = get_exe_dir();
	if (!exe_dir) {
		fputs("Failed to find executable path\n", stderr);
		exit(1);
	}
	char *path = alloc_concat(exe_dir, "/rom.db");
	tern_node *db = parse_config_file(path);
	free(path);
	if (!db) {
		fputs("Failed to load ROM DB\n", stderr);
	}
	return db;
}

char *get_header_name(uint8_t *rom)
{
	uint8_t *last = rom + TITLE_END - 1;
	uint8_t *src = rom + TITLE_START;
	
	while (last > src && (*last <=  0x20 || *last >= 0x80))
	{
		last--;
	}
	if (last == src) {
		//TODO: Use other name field
		return strdup("UNKNOWN");
	} else {
		last++;
		char *ret = malloc(last - (rom + TITLE_START) + 1);
		uint8_t *dst;
		for (dst = ret; src < last; src++)
		{
			if (*src >= 0x20 && *src < 0x80) {
				*(dst++) = *src;
			}
		}
		*dst = 0;
		return ret;
	}
}

char *region_chars = "UB4JEA";
uint8_t region_bits[] = {REGION_U, REGION_U, REGION_U, REGION_J, REGION_E, REGION_E};

uint8_t translate_region_char(uint8_t c)
{
	for (int i = 0; i < sizeof(region_bits); i++)
	{
		if (c == region_chars[i]) {
			return region_bits[i];
		}
	}
	return 0;
}

uint8_t get_header_regions(uint8_t *rom)
{
	uint8_t regions = 0;
	for (int i = 0; i < 3; i++)
	{
		regions |= translate_region_char(rom[REGION_START + i]);
	}
	return regions;
}

uint32_t get_u32be(uint8_t *data)
{
	return *data << 24 | data[1] << 16 | data[2] << 8 | data[3];
}

uint32_t calc_mask(uint32_t src_size, uint32_t start, uint32_t end)
{
	uint32_t map_size = end-start+1;
	if (src_size < map_size) {
		return nearest_pow2(src_size)-1;
	} else if (!start) {
		return 0xFFFFFF;
	} else {
		return nearest_pow2(map_size)-1;
	}
}

void add_memmap_header(rom_info *info, uint8_t *rom, uint32_t size, memmap_chunk const *base_map, int base_chunks)
{
	if (rom[RAM_ID] == 'R' && rom[RAM_ID+1] == 'A') {
		uint32_t rom_end = get_u32be(rom + ROM_END) + 1;
		uint32_t ram_start = get_u32be(rom + RAM_START);
		uint32_t ram_end = get_u32be(rom + RAM_END);
		uint32_t ram_flags = info->save_type = rom[RAM_FLAGS] & RAM_FLAG_MASK;
		ram_start &= 0xFFFFFE;
		ram_end |= 1;
		info->save_mask = ram_end - ram_start;
		uint32_t size = info->save_mask + 1;
		if (ram_flags != RAM_FLAG_BOTH) {
			size /= 2;
		}
		info->save_size = size;
		info->save_buffer = malloc(size);
		
		info->map_chunks = base_chunks + (ram_start >= rom_end ? 2 : 3);
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk)*2);
		memcpy(info->map+2, base_map, sizeof(memmap_chunk) * base_chunks);
		
		if (ram_start >= rom_end) {
			info->map[0].end = rom_end;
			//TODO: ROM mirroring
			info->map[0].mask = 0xFFFFFF;
			info->map[0].flags = MMAP_READ;
			info->map[0].buffer = rom;
			
			info->map[1].start = ram_start;
			info->map[1].mask = info->save_mask;
			info->map[1].end = ram_end + 1;
			info->map[1].flags = MMAP_READ | MMAP_WRITE;
			
			if (ram_flags == RAM_FLAG_ODD) {
				info->map[1].flags |= MMAP_ONLY_ODD;
			} else if (ram_flags == RAM_FLAG_EVEN) {
				info->map[1].flags |= MMAP_ONLY_EVEN;
			}
			info->map[1].buffer = info->save_buffer;
		} else {
			//Assume the standard Sega mapper
			info->map[0].end = 0x200000;
			info->map[0].mask = 0xFFFFFF;
			info->map[0].flags = MMAP_READ;
			info->map[0].buffer = rom;
			
			info->map[1].start = 0x200000;
			info->map[1].end = 0x400000;
			info->map[1].mask = 0x1FFFFF;
			info->map[1].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_FUNC_NULL;
			info->map[1].ptr_index = 2;
			info->map[1].read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[2] == NULL
			info->map[1].read_8 = (read_8_fun)read_sram_b;
			info->map[1].write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
			info->map[1].write_8 = (write_8_fun)write_sram_area_b;
			info->map[1].buffer = cart + 0x200000;
			
			memmap_chunk *last = info->map + info->map_chunks - 1;
			memset(last, 0, sizeof(memmap_chunk));
			last->start = 0xA13000;
			last->end = 0xA13100;
			last->mask = 0xFF;
			last->write_16 = (write_16_fun)write_bank_reg_w;
			last->write_8 = (write_8_fun)write_bank_reg_b;
		}
	} else {
		info->map_chunks = base_chunks + 1;
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk));
		memcpy(info->map+1, base_map, sizeof(memmap_chunk) * base_chunks);
		
		info->map[0].end = 0x400000;
		info->map[0].mask = 0xFFFFFF;
		info->map[0].flags = MMAP_READ;
		info->map[0].buffer = rom;
		info->save_type = SAVE_NONE;
	}
}

rom_info configure_rom_heuristics(uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	rom_info info;
	info.name = get_header_name(rom);
	info.regions = get_header_regions(rom);
	add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
	return info;
}

typedef struct {
	rom_info     *info;
	uint8_t      *rom;
	tern_node    *root;
	uint32_t     rom_size;
	int          index;
	int          num_els;
} map_iter_state;

void eeprom_read_fun(char *key, tern_val val, void *data)
{
	int bit = atoi(key);
	if (bit < 0 || bit > 15) {
		fprintf(stderr, "bit %s is out of range", key);
		return;
	}
	char *pin = val.ptrval;
	if (strcmp(pin, "sda")) {
		fprintf(stderr, "bit %s is connected to unrecognized read pin %s", key, pin);
		return;
	}
	eeprom_map *map = data;
	map->sda_read_bit = bit;
}

void eeprom_write_fun(char *key, tern_val val, void *data)
{
	int bit = atoi(key);
	if (bit < 0 || bit > 15) {
		fprintf(stderr, "bit %s is out of range", key);
		return;
	}
	char *pin = val.ptrval;
	eeprom_map *map = data;
	if (!strcmp(pin, "sda")) {
		map->sda_write_mask = 1 << bit;
		return;
	}
	if (!strcmp(pin, "scl")) {
		map->scl_mask = 1 << bit;
		return;
	}
	fprintf(stderr, "bit %s is connected to unrecognized write pin %s", key, pin);
}

void map_iter_fun(char *key, tern_val val, void *data)
{
	map_iter_state *state = data;
	tern_node *node = tern_get_node(val);
	if (!node) {
		fprintf(stderr, "ROM DB map entry %d with address %s is not a node\n", state->index, key);
		exit(1);
	}
	uint32_t start = strtol(key, NULL, 16);
	uint32_t end = strtol(tern_find_ptr_default(node, "last", "0"), NULL, 16);
	if (!end || end < start) {
		fprintf(stderr, "'last' value is missing or invalid for ROM DB map entry %d with address %s\n", state->index, key);
		exit(1);
	}
	char * dtype = tern_find_ptr_default(node, "device", "ROM");
	uint32_t offset = strtol(tern_find_ptr_default(node, "offset", "0"), NULL, 0);
	memmap_chunk *map = state->info->map + state->index;
	map->start = start;
	map->end = end;
	if (!strcmp(dtype, "ROM")) {
		map->buffer = state->rom + offset;
		map->flags = MMAP_READ;
		map->mask = calc_mask(state->rom_size, start, end);
	} else if (!strcmp(dtype, "EEPROM")) {
		if (!state->info->save_size) {
			char * size = tern_find_path(state->root, "EEPROM\0size\0").ptrval;
			if (!size) {
				fprintf(stderr, "ROM DB map entry %d with address %s has device type EEPROM, but the EEPROM size is not defined\n", state->index, key);
				exit(1);
			}
			state->info->save_size = atoi(size);
			if (!state->info->save_size) {
				fprintf(stderr, "EEPROM size %s is invalid\n", size);
				exit(1);
			}
			char *etype = tern_find_path(state->root, "EEPROM\0type\0").ptrval;
			if (!etype) {
				etype = "i2c";
			}
			if (!strcmp(etype, "i2c")) {
				state->info->save_type = SAVE_I2C;
			} else {
				fprintf(stderr, "EEPROM type %s is invalid\n", etype);
				exit(1);
			}
			state->info->save_buffer = malloc(state->info->save_size);
			state->info->eeprom_map = malloc(sizeof(eeprom_map) * state->num_els);
			memset(state->info->eeprom_map, 0, sizeof(eeprom_map) * state->num_els);
		}
		eeprom_map *eep_map = state->info->eeprom_map + state->info->num_eeprom;
		eep_map->start = start;
		eep_map->end = end;
		eep_map->sda_read_bit = 0xFF;
		tern_node * bits_read = tern_find_ptr(node, "bits_read");
		if (bits_read) {
			tern_foreach(bits_read, eeprom_read_fun, eep_map);
		}
		tern_node * bits_write = tern_find_ptr(node, "bits_write");
		if (bits_write) {
			tern_foreach(bits_write, eeprom_write_fun, eep_map);
		}
		printf("EEPROM address %X: sda read: %X, sda write: %X, scl: %X\n", start, eep_map->sda_read_bit, eep_map->sda_write_mask, eep_map->scl_mask);
		state->info->num_eeprom++;
		map->write_16 = write_eeprom_i2c_w;
		map->write_8 = write_eeprom_i2c_b;
		map->read_16 = read_eeprom_i2c_w;
		map->read_8 = read_eeprom_i2c_b;
		map->mask = 0xFFFFFF;
	} else if (!strcmp(dtype, "SRAM")) {
		if (!state->info->save_size) {
			char * size = tern_find_path(state->root, "SRAM\0size\0").ptrval;
			if (!size) {
				fprintf(stderr, "ROM DB map entry %d with address %s has device type SRAM, but the SRAM size is not defined\n", state->index, key);
				exit(1);
			}
			state->info->save_size = atoi(size);
			if (!state->info->save_size) {
				fprintf(stderr, "SRAM size %s is invalid\n", size);
				exit(1);
			}
			state->info->save_buffer = malloc(state->info->save_size);
			char *bus = tern_find_path(state->root, "SRAM\0bus\0").ptrval;
			if (!strcmp(bus, "odd")) {
				state->info->save_type = RAM_FLAG_ODD;
			} else if(!strcmp(bus, "even")) {
				state->info->save_type = RAM_FLAG_EVEN;
			} else {
				state->info->save_type = RAM_FLAG_BOTH;
			}
		}
		map->buffer = state->info->save_buffer + offset;
		map->flags = MMAP_READ | MMAP_WRITE;
		if (state->info->save_type == RAM_FLAG_ODD) {
			map->flags |= MMAP_ONLY_ODD;
		} else if(state->info->save_type == RAM_FLAG_EVEN) {
			map->flags |= MMAP_ONLY_EVEN;
		}
		map->mask = calc_mask(state->info->save_size, start, end);
	} else {
		fprintf(stderr, "Invalid device type for ROM DB map entry %d with address %s\n", state->index, key);
		exit(1);
	}
	state->index++;
}

rom_info configure_rom(tern_node *rom_db, void *vrom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	uint8_t product_id[GAME_ID_LEN+1];
	uint8_t *rom = vrom;
	product_id[GAME_ID_LEN] = 0;
	for (int i = 0; i < GAME_ID_LEN; i++)
	{
		if (rom[GAME_ID_OFF + i] <= ' ') {
			product_id[i] = 0;
			break;
		}
		product_id[i] = rom[GAME_ID_OFF + i];
		
	}
	printf("Product ID: %s\n", product_id);
	tern_node * entry = tern_find_ptr(rom_db, product_id);
	if (!entry) {
		puts("Not found in ROM DB, examining header\n");
		return configure_rom_heuristics(rom, rom_size, base_map, base_chunks);
	}
	rom_info info;
	info.name = tern_find_ptr(entry, "name");
	if (info.name) {
		printf("Found name: %s\n", info.name);
		info.name = strdup(info.name);
	} else {
		info.name = get_header_name(rom);
	}
	
	char *dbreg = tern_find_ptr(entry, "regions");
	info.regions = 0;
	if (dbreg) {
		while (*dbreg != 0)
		{
			info.regions |= translate_region_char(*(dbreg++));
		}
	}
	if (!info.regions) {
		info.regions = get_header_regions(rom);
	}
	
	tern_node *map = tern_find_ptr(entry, "map");
	if (map) {
		info.map_chunks = tern_count(map);
		if (info.map_chunks) {
			info.map_chunks += base_chunks;
			info.save_buffer = NULL;
			info.save_size = 0;
			info.map = malloc(sizeof(memmap_chunk) * info.map_chunks);
			info.eeprom_map = NULL;
			info.num_eeprom = 0;
			memset(info.map, 0, sizeof(memmap_chunk) * (info.map_chunks - base_chunks));
			map_iter_state state = {&info, rom, entry, rom_size, 0, info.map_chunks - base_chunks};
			tern_foreach(map, map_iter_fun, &state);
			memcpy(info.map + state.index, base_map, sizeof(memmap_chunk) * base_chunks);
		} else {
			add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
		}
	} else {
		add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
	}
	
	return info;
}
