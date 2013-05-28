#include "68kinst.h"
#include "m68k_to_x86.h"
#include "z80_to_x86.h"
#include "mem.h"
#include "vdp.h"
#include "render.h"
#include "blastem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARTRIDGE_WORDS 0x200000
#define RAM_WORDS 32 * 1024
#define Z80_RAM_BYTES 8 * 1024
#define MCLKS_PER_68K 7
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)
//TODO: Figure out the exact value for this
#define CYCLE_NEVER 0xFFFFFFFF
#define LINES_NTSC 262
#define LINES_PAL 312

uint32_t mclks_per_frame = MCLKS_LINE*LINES_NTSC;

uint16_t cart[CARTRIDGE_WORDS];
uint16_t ram[RAM_WORDS];
uint8_t z80_ram[Z80_RAM_BYTES];

io_port gamepad_1;
io_port gamepad_2;

int headless = 0;
int z80_enabled = 1;
int frame_limit = 0;

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#define SMD_HEADER_SIZE 512
#define SMD_MAGIC1 0x03
#define SMD_MAGIC2 0xAA
#define SMD_MAGIC3 0xBB
#define SMD_BLOCK_SIZE 0x4000

int load_smd_rom(long filesize, FILE * f)
{
	uint8_t block[SMD_BLOCK_SIZE];
	filesize -= SMD_HEADER_SIZE;
	fseek(f, SMD_HEADER_SIZE, SEEK_SET);
	
	uint16_t * dst = cart;
	while (filesize > 0) {
		fread(block, 1, SMD_BLOCK_SIZE, f);
		for (uint8_t *low = block, *high = (block+SMD_BLOCK_SIZE/2), *end = block+SMD_BLOCK_SIZE; high < end; high++, low++) {
			*(dst++) = *high << 8 | *low;
		}
		filesize -= SMD_BLOCK_SIZE;
	}
	return 1;
}

int load_rom(char * filename)
{
	uint8_t header[10];
	FILE * f = fopen(filename, "rb");
	if (!f) {
		return 0;
	}
	fread(header, 1, sizeof(header), f);
	fseek(f, 0, SEEK_END);
	long filesize = ftell(f);
	if (filesize/2 > CARTRIDGE_WORDS) {
		//carts bigger than 4MB not currently supported
		filesize = CARTRIDGE_WORDS*2;
	}
	fseek(f, 0, SEEK_SET);
	if (header[1] == SMD_MAGIC1 && header[8] == SMD_MAGIC2 && header[9] == SMD_MAGIC3) {
		int i;
		for (i = 3; i < 8; i++) {
			if (header[i] != 0) {
				break;
			}
		}
		if (i == 8) {
			if (header[2]) {
				fprintf(stderr, "%s is a split SMD ROM which is not currently supported", filename);
				exit(1);
			}
			return load_smd_rom(filesize, f);
		}
	}
	fread(cart, 2, filesize/2, f);
	fclose(f);
	for(unsigned short * cur = cart; cur - cart < (filesize/2); ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
	//TODO: Mirror ROM
	return 1;
}

uint16_t read_dma_value(uint32_t address)
{
	//addresses here are word addresses (i.e. bit 0 corresponds to A1), so no need to do div by 2
	if (address < 0x200000) {
		return cart[address];
	} else if(address >= 0x700000) {
		return ram[address & 0x7FFF];
	}
	//TODO: Figure out what happens when you try to DMA from weird adresses like IO or banked Z80 area
	return 0;
}

//TODO: Make these dependent on the video mode
//#define VINT_CYCLE ((MCLKS_LINE * 225 + (148 + 40) * 4)/MCLKS_PER_68K)
#define ZVINT_CYCLE ((MCLKS_LINE * 225 + (148 + 40) * 4)/MCLKS_PER_Z80)
//#define VINT_CYCLE ((MCLKS_LINE * 226)/MCLKS_PER_68K)
//#define ZVINT_CYCLE ((MCLKS_LINE * 226)/MCLKS_PER_Z80)

void adjust_int_cycle(m68k_context * context, vdp_context * v_context)
{
	context->int_cycle = CYCLE_NEVER;
	if ((context->status & 0x7) < 6) {
		uint32_t next_vint = vdp_next_vint(v_context);
		if (next_vint != CYCLE_NEVER) {
			next_vint /= MCLKS_PER_68K;
			context->int_cycle = next_vint;
			context->int_num = 6;
		}
		if ((context->status & 0x7) < 4) {
			uint32_t next_hint = vdp_next_hint(v_context);
			if (next_hint != CYCLE_NEVER) {
				next_hint /= MCLKS_PER_68K;
				if (next_hint < context->int_cycle) {
					context->int_cycle = next_hint;
					context->int_num = 4;
			
				}
			}
		}
	}

	context->target_cycle = context->int_cycle < context->sync_cycle ? context->int_cycle : context->sync_cycle;
	/*printf("Cyc: %d, Trgt: %d, Int Cyc: %d, Int: %d, Mask: %X, V: %d, H: %d, HICount: %d, HReg: %d, Line: %d\n", 
		context->current_cycle, context->target_cycle, context->int_cycle, context->int_num, (context->status & 0x7), 
		v_context->regs[REG_MODE_2] & 0x20, v_context->regs[REG_MODE_1] & 0x10, v_context->hint_counter, v_context->regs[REG_HINT], v_context->cycles / MCLKS_LINE);*/
}

int break_on_sync = 0;

uint8_t reset = 1;
uint8_t need_reset = 0;
uint8_t busreq = 0;
uint8_t busack = 0;
uint32_t busack_cycle = CYCLE_NEVER;
uint8_t new_busack = 0;
//#define DO_DEBUG_PRINT
#ifdef DO_DEBUG_PRINT
#define dprintf printf
#define dputs puts
#else
#define dprintf
#define dputs
#endif

void sync_z80(z80_context * z_context, uint32_t mclks)
{
	if (z80_enabled && !reset && !busreq) {
		genesis_context * gen = z_context->system;
		if (need_reset) {
			z80_reset(z_context);
			need_reset = 0;
		}
		z_context->sync_cycle = mclks / MCLKS_PER_Z80;
		uint32_t vint_cycle = vdp_next_vint_z80(gen->vdp) / MCLKS_PER_Z80;
		while (z_context->current_cycle < z_context->sync_cycle) {
			if (z_context->iff1 && z_context->current_cycle < vint_cycle) {
				z_context->int_cycle = vint_cycle < z_context->int_enable_cycle ? z_context->int_enable_cycle : vint_cycle;
			}
			z_context->target_cycle = z_context->sync_cycle < z_context->int_cycle ? z_context->sync_cycle : z_context->int_cycle;
			dprintf("Running Z80 from cycle %d to cycle %d. Native PC: %p\n", z_context->current_cycle, z_context->sync_cycle, z_context->native_pc);
			z80_run(z_context);
			dprintf("Z80 ran to cycle %d\n", z_context->current_cycle);
		}
	} else {
		z_context->current_cycle = mclks / MCLKS_PER_Z80;
	}
}
uint32_t frame=0;
m68k_context * sync_components(m68k_context * context, uint32_t address)
{
	//TODO: Handle sync targets smaller than a single frame
	genesis_context * gen = context->system;
	vdp_context * v_context = gen->vdp;
	z80_context * z_context = gen->z80;
	uint32_t mclks = context->current_cycle * MCLKS_PER_68K;
	sync_z80(z_context, mclks);
	if (mclks >= mclks_per_frame) {
		ym_run(gen->ym, context->current_cycle);
		gen->ym->current_cycle -= ((mclks_per_frame/MCLKS_PER_68K) / 6) * 6;
		//printf("reached frame end | 68K Cycles: %d, MCLK Cycles: %d\n", context->current_cycle, mclks);
		vdp_run_context(v_context, mclks_per_frame);
		psg_run(gen->psg, mclks/MCLKS_PER_PSG);
		gen->psg->cycles -= mclks_per_frame/MCLKS_PER_PSG;
		if (!headless) {
			break_on_sync |= wait_render_frame(v_context, frame_limit);
		}
		frame++;
		mclks -= mclks_per_frame;
		vdp_adjust_cycles(v_context, mclks_per_frame);
		io_adjust_cycles(&gamepad_1, context->current_cycle, mclks_per_frame/MCLKS_PER_68K);
		io_adjust_cycles(&gamepad_2, context->current_cycle, mclks_per_frame/MCLKS_PER_68K);
		if (busack_cycle != CYCLE_NEVER) {
			if (busack_cycle > mclks_per_frame/MCLKS_PER_68K) {
				busack_cycle -= mclks_per_frame/MCLKS_PER_68K;
			} else {
				busack_cycle = CYCLE_NEVER;
				busack = new_busack;
			}
		}
		context->current_cycle -= mclks_per_frame/MCLKS_PER_68K;
		if (z_context->current_cycle >= mclks_per_frame/MCLKS_PER_Z80) {
			z_context->current_cycle -= mclks_per_frame/MCLKS_PER_Z80;
		} else {
			z_context->current_cycle = 0;
		}
		if (mclks) {
			vdp_run_context(v_context, mclks);
		}
	} else {
		//printf("running VDP for %d cycles\n", mclks - v_context->cycles);
		vdp_run_context(v_context, mclks);
		psg_run(gen->psg, mclks/MCLKS_PER_PSG);
	}
	if (context->int_ack) {
		vdp_int_ack(v_context, context->int_ack);
		context->int_ack = 0;
	}
	adjust_int_cycle(context, v_context);
	if (break_on_sync && address) {
		break_on_sync = 0;
		debugger(context, address);
	}
	return context;
}

m68k_context * vdp_port_write(uint32_t vdp_port, m68k_context * context, uint16_t value)
{
	if (vdp_port & 0x2700E0) {
		printf("machine freeze due to write to address %X\n", 0xC00000 | vdp_port);
		exit(1);
	}
	vdp_port &= 0x1F;
	//printf("vdp_port write: %X, value: %X, cycle: %d\n", vdp_port, value, context->current_cycle);
	sync_components(context, 0);
	vdp_context * v_context = context->video_context;
	if (vdp_port < 0x10) {
		int blocked;
		uint32_t before_cycle = v_context->cycles;
		if (vdp_port < 4) {
			while (vdp_data_port_write(v_context, value) < 0) {
				while(v_context->flags & FLAG_DMA_RUN) {
					vdp_run_dma_done(v_context, mclks_per_frame);
					if (v_context->cycles >= mclks_per_frame) {
						if (!headless) {
							wait_render_frame(v_context, frame_limit);
						}
						vdp_adjust_cycles(v_context, mclks_per_frame);
						io_adjust_cycles(&gamepad_1, v_context->cycles/MCLKS_PER_68K, mclks_per_frame/MCLKS_PER_68K);
						io_adjust_cycles(&gamepad_2, v_context->cycles/MCLKS_PER_68K, mclks_per_frame/MCLKS_PER_68K);
						if (busack_cycle != CYCLE_NEVER) {
							if (busack_cycle > mclks_per_frame/MCLKS_PER_68K) {
								busack_cycle -= mclks_per_frame/MCLKS_PER_68K;
							} else {
								busack_cycle = CYCLE_NEVER;
								busack = new_busack;
							}
						}
					}
				}
				context->current_cycle = v_context->cycles / MCLKS_PER_68K;
			}
		} else if(vdp_port < 8) {
			blocked = vdp_control_port_write(v_context, value);
			if (blocked) {
				while (blocked) {
					while(v_context->flags & FLAG_DMA_RUN) {
						vdp_run_dma_done(v_context, mclks_per_frame);
						if (v_context->cycles >= mclks_per_frame) {
							if (!headless) {
								wait_render_frame(v_context, frame_limit);
							}
							vdp_adjust_cycles(v_context, mclks_per_frame);
							io_adjust_cycles(&gamepad_1, v_context->cycles/MCLKS_PER_68K, mclks_per_frame/MCLKS_PER_68K);
							io_adjust_cycles(&gamepad_2, v_context->cycles/MCLKS_PER_68K, mclks_per_frame/MCLKS_PER_68K);
							if (busack_cycle != CYCLE_NEVER) {
								if (busack_cycle > mclks_per_frame/MCLKS_PER_68K) {
									busack_cycle -= mclks_per_frame/MCLKS_PER_68K;
								} else {
									busack_cycle = CYCLE_NEVER;
									busack = new_busack;
								}
							}
						}
					}
					if (blocked < 0) {
						blocked = vdp_control_port_write(v_context, value);
					} else {
						blocked = 0;
					}
				}
				context->current_cycle = v_context->cycles / MCLKS_PER_68K;
			} else {
				adjust_int_cycle(context, v_context);
			}
		} else {
			printf("Illegal write to HV Counter port %X\n", vdp_port);
			exit(1);
		}
		if (v_context->cycles != before_cycle) {
			context->current_cycle = v_context->cycles / MCLKS_PER_68K;
		}
	} else if (vdp_port < 0x18) {
		genesis_context * gen = context->system;
		psg_run(gen->psg, (context->current_cycle * MCLKS_PER_68K) / MCLKS_PER_PSG);
		psg_write(gen->psg, value);
	} else {
		//TODO: Implement undocumented test register(s)
	}
	return context;
}

m68k_context * vdp_port_write_b(uint32_t vdp_port, m68k_context * context, uint8_t value)
{
	return vdp_port_write(vdp_port, context, vdp_port < 0x10 ? value | value << 8 : ((vdp_port & 1) ? value : 0));
}

z80_context * z80_vdp_port_write(uint16_t vdp_port, z80_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	if (vdp_port & 0xE0) {
		printf("machine freeze due to write to Z80 address %X\n", 0x7F00 | vdp_port);
		exit(1);
	}
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		vdp_run_context(gen->vdp, context->current_cycle * MCLKS_PER_Z80);
		if (vdp_port < 4) {
			vdp_data_port_write(gen->vdp, value << 8 | value);
		} else if (vdp_port < 8) {
			vdp_control_port_write(gen->vdp, value << 8 | value);
		} else {
			printf("Illegal write to HV Counter port %X\n", vdp_port);
			exit(1);
		}
	} else if (vdp_port < 0x18) {
		psg_run(gen->psg, (context->current_cycle * MCLKS_PER_Z80) / MCLKS_PER_PSG);
		psg_write(gen->psg, value);
	} else {
		//TODO: Implement undocumented test register(s)
	}
	return context;
}

uint16_t vdp_port_read(uint32_t vdp_port, m68k_context * context)
{
	if (vdp_port & 0x2700E0) {
		printf("machine freeze due to read from address %X\n", 0xC00000 | vdp_port);
		exit(1);
	}
	vdp_port &= 0x1F;
	uint16_t value;
	sync_components(context, 0);
	vdp_context * v_context = context->video_context;
	if (vdp_port < 0x10) {
		if (vdp_port < 4) {
			value = vdp_data_port_read(v_context);
		} else if(vdp_port < 8) {
			value = vdp_control_port_read(v_context);
		} else {
			value = vdp_hv_counter_read(v_context);
			//printf("HV Counter: %X at cycle %d\n", value, v_context->cycles);
		}
		context->current_cycle = v_context->cycles/MCLKS_PER_68K;
	} else {
		printf("Illegal read from PSG or test register port %X\n", vdp_port);
		exit(1);
	}
	return value;
}

uint8_t vdp_port_read_b(uint32_t vdp_port, m68k_context * context)
{
	uint16_t value = vdp_port_read(vdp_port, context);
	if (vdp_port & 1) {
		return value;
	} else {
		return value >> 8;
	}
}

#define TH 0x40
#define TH_TIMEOUT 8000

void io_adjust_cycles(io_port * pad, uint32_t current_cycle, uint32_t deduction)
{
	/*uint8_t control = pad->control | 0x80;
	uint8_t th = control & pad->output;
	if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf("adjust_cycles | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, pad->input[GAMEPAD_TH0], pad->input[GAMEPAD_TH1], pad->th_counter,pad->timeout_cycle, current_cycle);
	}*/
	if (current_cycle >= pad->timeout_cycle) {
		pad->th_counter = 0;
	} else {
		pad->timeout_cycle -= deduction;
	}
	if (busack_cycle < CYCLE_NEVER && current_cycle < busack_cycle) {
		busack_cycle -= deduction;
	}
}

void io_data_write(io_port * pad, m68k_context * context, uint8_t value)
{
	if (pad->control & TH) {
		//check if TH has changed
		if ((pad->output & TH) ^ (value & TH)) {
			if (context->current_cycle >= pad->timeout_cycle) {
				pad->th_counter = 0;
			}
			if (!(value & TH)) {
				pad->th_counter++;
			}
			pad->timeout_cycle = context->current_cycle + TH_TIMEOUT;
		}
	}
	pad->output = value;
}

uint8_t io_data_read(io_port * pad, m68k_context * context)
{
	uint8_t control = pad->control | 0x80;
	uint8_t th = control & pad->output;
	uint8_t input;
	if (context->current_cycle >= pad->timeout_cycle) {
		pad->th_counter = 0;
	}
	/*if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf("io_data_read | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, pad->input[GAMEPAD_TH0], pad->input[GAMEPAD_TH1], pad->th_counter,pad->timeout_cycle, context->current_cycle);
	}*/
	if (th) {
		if (pad->th_counter == 3) {
			input = pad->input[GAMEPAD_EXTRA];
		} else {
			input = pad->input[GAMEPAD_TH1];
		}
	} else {
		if (pad->th_counter == 3) {
			input = pad->input[GAMEPAD_TH0] | 0xF;
		} else if(pad->th_counter == 4) {
			input = pad->input[GAMEPAD_TH0]  & 0x30;
		} else {
			input = pad->input[GAMEPAD_TH0] | 0xC;
		}
	}
	uint8_t value = ((~input) & (~control)) | (pad->output & control);
	/*if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf ("value: %X\n", value);
	}*/
	return value;
}

uint32_t zram_counter = 0;
#define Z80_ACK_DELAY 3
#define Z80_BUSY_DELAY 1//TODO: Find the actual value for this
#define Z80_REQ_BUSY 1
#define Z80_REQ_ACK 0
#define Z80_RES_BUSACK reset

m68k_context * io_write(uint32_t location, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	if (location < 0x10000) {
		if (busack_cycle <= context->current_cycle) {
			busack = new_busack;
			busack_cycle = CYCLE_NEVER;
		}
		if (!(busack || reset)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				z80_ram[location & 0x1FFF] = value;
				z80_handle_code_write(location & 0x1FFF, gen->z80);
			} else if (location < 0x6000) {
				ym_run(gen->ym, context->current_cycle);
				if (location & 1) {
					ym_data_write(gen->ym, value);
				} else if(location & 2) {
					ym_address_write_part2(gen->ym, value);
				} else {
					ym_address_write_part1(gen->ym, value);
				}
			}
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x1:
				io_data_write(&gamepad_1, context, value);
				break;
			case 0x2:
				io_data_write(&gamepad_2, context, value);
				break;
			case 0x3://PORT C Data
				break;
			case 0x4:
				gamepad_1.control = value;
				break;
			case 0x5:
				gamepad_2.control = value;
				break;
			}
		} else {
			if (location == 0x1100) {
				sync_z80(gen->z80, context->current_cycle * MCLKS_PER_68K);
				if (busack_cycle <= context->current_cycle) {
					busack = new_busack;
					busack_cycle = CYCLE_NEVER;
				}
				if (value & 1) {
					dputs("bus requesting Z80");
					
					if(!reset && !busreq) {
						busack_cycle = ((gen->z80->current_cycle + Z80_ACK_DELAY) * MCLKS_PER_Z80) / MCLKS_PER_68K;//context->current_cycle + Z80_ACK_DELAY;
						new_busack = Z80_REQ_ACK;
						busreq = 1;
					}
				} else {
					if (busreq) {
						dputs("releasing z80 bus");
						#ifdef DO_DEBUG_PRINT
						char fname[20];
						sprintf(fname, "zram-%d", zram_counter++);
						FILE * f = fopen(fname, "wb");
						fwrite(z80_ram, 1, sizeof(z80_ram), f);
						fclose(f);
						#endif
						busack_cycle = ((gen->z80->current_cycle + Z80_BUSY_DELAY) * MCLKS_PER_Z80) / MCLKS_PER_68K;
						new_busack = Z80_REQ_BUSY;
						busreq = 0;
					}
					//busack_cycle = CYCLE_NEVER;
					//busack = Z80_REQ_BUSY;
					
				}
			} else if (location == 0x1200) {
				sync_z80(gen->z80, context->current_cycle * MCLKS_PER_68K);
				if (value & 1) {
					if (reset && busreq) {
						new_busack = 0;
						busack_cycle = ((gen->z80->current_cycle + Z80_ACK_DELAY) * MCLKS_PER_Z80) / MCLKS_PER_68K;//context->current_cycle + Z80_ACK_DELAY;
					}
					//TODO: Deal with the scenario in which reset is not asserted long enough
					if (reset) {
						need_reset = 1;
						//TODO: Add necessary delay between release of reset and start of execution
						gen->z80->current_cycle = (context->current_cycle * MCLKS_PER_68K) / MCLKS_PER_Z80;
					}
					reset = 0;
				} else {
					reset = 1;
				}
			}
		}
	}
	return context;
}

m68k_context * io_write_w(uint32_t location, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	if (location < 0x10000) {
		if (busack_cycle <= context->current_cycle) {
			busack = new_busack;
			busack_cycle = CYCLE_NEVER;
		}
		if (!(busack || reset)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				z80_ram[location & 0x1FFE] = value >> 8;
				z80_handle_code_write(location & 0x1FFE, gen->z80);
			} else if (location < 0x6000) {
				ym_run(gen->ym, context->current_cycle);
				if (location & 1) {
					ym_data_write(gen->ym, value >> 8);
				} else if(location & 2) {
					ym_address_write_part2(gen->ym, value >> 8);
				} else {
					ym_address_write_part1(gen->ym, value >> 8);
				}
			}
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x1:
				io_data_write(&gamepad_1, context, value);
				break;
			case 0x2:
				io_data_write(&gamepad_2, context, value);
				break;
			case 0x3://PORT C Data
				break;
			case 0x4:
				gamepad_1.control = value;
				break;
			case 0x5:
				gamepad_2.control = value;
				break;
			}
		} else {
			//printf("IO Write of %X to %X @ %d\n", value, location, context->current_cycle);
			if (location == 0x1100) {
				sync_z80(gen->z80, context->current_cycle * MCLKS_PER_68K);
				if (busack_cycle <= context->current_cycle) {
					busack = new_busack;
					busack_cycle = CYCLE_NEVER;
				}
				if (value & 0x100) {
					dprintf("bus requesting Z80 @ %d\n", (context->current_cycle * MCLKS_PER_68K) / MCLKS_PER_Z80);
					
					if(!reset && !busreq) {
						busack_cycle = ((gen->z80->current_cycle + Z80_ACK_DELAY) * MCLKS_PER_Z80) / MCLKS_PER_68K;//context->current_cycle + Z80_ACK_DELAY;
						new_busack = Z80_REQ_ACK;
						busreq = 1;
					}
				} else {
					if (busreq) {
						dprintf("releasing Z80 bus @ %d\n", (context->current_cycle * MCLKS_PER_68K) / MCLKS_PER_Z80);
						#ifdef DO_DEBUG_PRINT
						char fname[20];
						sprintf(fname, "zram-%d", zram_counter++);
						FILE * f = fopen(fname, "wb");
						fwrite(z80_ram, 1, sizeof(z80_ram), f);
						fclose(f);
						#endif
						busack_cycle = ((gen->z80->current_cycle + Z80_BUSY_DELAY) * MCLKS_PER_Z80) / MCLKS_PER_68K;
						new_busack = Z80_REQ_BUSY;
						busreq = 0;
					}
					//busack_cycle = CYCLE_NEVER;
					//busack = Z80_REQ_BUSY;
				}
			} else if (location == 0x1200) {
				sync_z80(gen->z80, context->current_cycle * MCLKS_PER_68K);
				if (value & 0x100) {
					if (reset && busreq) {
						new_busack = 0;
						busack_cycle = ((gen->z80->current_cycle + Z80_ACK_DELAY) * MCLKS_PER_Z80) / MCLKS_PER_68K;//context->current_cycle + Z80_ACK_DELAY;
					}
					//TODO: Deal with the scenario in which reset is not asserted long enough
					if (reset) {
						need_reset = 1;
						//TODO: Add necessary delay between release of reset and start of execution
						gen->z80->current_cycle = (context->current_cycle * MCLKS_PER_68K) / MCLKS_PER_Z80;
					}
					reset = 0;
				} else {
					reset = 1;
				}
			}
		}
	}
	return context;
}

#define USA 0x80
#define JAP 0x00
#define EUR 0xC0
#define NO_DISK 0x20
uint8_t version_reg = NO_DISK | USA;

uint8_t io_read(uint32_t location, m68k_context * context)
{
	uint8_t value;
	genesis_context *gen = context->system;
	if (location < 0x10000) {
		if (busack_cycle <= context->current_cycle) {
			busack = new_busack;
			busack_cycle = CYCLE_NEVER;
		}
		if (!(busack==Z80_REQ_BUSY || reset)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				value = z80_ram[location & 0x1FFF];
			} else if (location < 0x6000) {
				ym_run(gen->ym, context->current_cycle);
				value = ym_read_status(gen->ym);
			} else {
				value = 0xFF;
			}
		} else {
			value = 0xFF;
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x0:
				//version bits should be 0 for now since we're not emulating TMSS
				value = version_reg;
				break;
			case 0x1:
				value = io_data_read(&gamepad_1, context);
				break;
			case 0x2:
				value = io_data_read(&gamepad_2, context);
				break;
			case 0x3://PORT C Data
				break;
			case 0x4:
				value = gamepad_1.control;
				break;
			case 0x5:
				value = gamepad_2.control;
				break;
			default:
				value = 0xFF;
			}
		} else {
			if (location == 0x1100) {
				if (busack_cycle <= context->current_cycle) {
					busack = new_busack;
					busack_cycle = CYCLE_NEVER;
				}
				value = Z80_RES_BUSACK || busack;
				dprintf("Byte read of BUSREQ returned %d @ %d (reset: %d, busack: %d, busack_cycle %d)\n", value, context->current_cycle, reset, busack, busack_cycle);
			} else if (location == 0x1200) {
				value = !reset;
			} else {
				value = 0xFF;
				printf("Byte read of unknown IO location: %X\n", location);
			}
		}
	}
	return value;
}

uint16_t io_read_w(uint32_t location, m68k_context * context)
{
	uint16_t value;
	genesis_context * gen = context->system;
	if (location < 0x10000) {
		if (busack_cycle <= context->current_cycle) {
			busack = new_busack;
			busack_cycle = CYCLE_NEVER;
		}
		if (!(busack==Z80_REQ_BUSY || reset)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				value = z80_ram[location & 0x1FFE];
			} else if (location < 0x6000) {
				ym_run(gen->ym, context->current_cycle);
				value = ym_read_status(gen->ym);	
			} else {
				value = 0xFF;
			}
			value = value | (value << 8);
		} else {
			value = 0xFFFF;
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x0:
				//version bits should be 0 for now since we're not emulating TMSS
				//Not sure about the other bits
				value = version_reg;
				break;
			case 0x1:
				value = io_data_read(&gamepad_1, context);
				break;
			case 0x2:
				value = io_data_read(&gamepad_2, context);
				break;
			case 0x3://PORT C Data
				break;
			case 0x4:
				value = gamepad_1.control;
				break;
			case 0x5:
				value = gamepad_2.control;
				break;
			case 0x6:
				//PORT C Control
				value = 0;
				break;
			default:
				value = 0;
			}
			value = value | (value << 8);
			//printf("Word read to %X returned %d\n", location, value);
		} else {
			if (location == 0x1100) {
				if (busack_cycle <= context->current_cycle) {
					busack = new_busack;
					busack_cycle = CYCLE_NEVER;
				}
				value = (Z80_RES_BUSACK || busack) << 8;
				//printf("Word read of BUSREQ returned %d\n", value);
			} else if (location == 0x1200) {
				value = (!reset) << 8;
			} else {
				printf("Word read of unknown IO location: %X\n", location);
			}
		}
	}
	return value;
}

z80_context * z80_write_ym(uint16_t location, z80_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	ym_run(gen->ym, (context->current_cycle * MCLKS_PER_Z80) / MCLKS_PER_68K);
	if (location & 1) {
		ym_data_write(gen->ym, value);
	} else if (location & 2) {
		ym_address_write_part2(gen->ym, value);
	} else {
		ym_address_write_part1(gen->ym, value);
	}
	return context;
}

uint8_t z80_read_ym(uint16_t location, z80_context * context)
{
	genesis_context * gen = context->system;
	ym_run(gen->ym, (context->current_cycle * MCLKS_PER_Z80) / MCLKS_PER_68K);
	return ym_read_status(gen->ym);
}

uint16_t read_sram_w(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_flags)
	{
	case RAM_FLAG_BOTH:
		return gen->save_ram[address] << 8 | gen->save_ram[address+1];
	case RAM_FLAG_EVEN:
		return gen->save_ram[address >> 1] << 8 | 0xFF;
	case RAM_FLAG_ODD:
		return gen->save_ram[address >> 1] | 0xFF00;
	}
	return 0xFFFF;//We should never get here
}

uint8_t read_sram_b(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_flags)
	{
	case RAM_FLAG_BOTH:
		return gen->save_ram[address];
	case RAM_FLAG_EVEN:
		if (address & 1) {
			return 0xFF;
		} else {
			return gen->save_ram[address >> 1];
		}
	case RAM_FLAG_ODD:
		if (address & 1) {
			return gen->save_ram[address >> 1];
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
		switch(gen->save_flags)
		{
		case RAM_FLAG_BOTH:
			gen->save_ram[address] = value >> 8;
			gen->save_ram[address+1] = value;
			break;
		case RAM_FLAG_EVEN:
			gen->save_ram[address >> 1] = value >> 8;
			break;
		case RAM_FLAG_ODD:
			gen->save_ram[address >> 1] = value;
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
		switch(gen->save_flags)
		{
		case RAM_FLAG_BOTH:
			gen->save_ram[address] = value;
			break;
		case RAM_FLAG_EVEN:
			if (!(address & 1)) {
				gen->save_ram[address >> 1] = value;
			}
			break;
		case RAM_FLAG_ODD:
			if (address & 1) {
				gen->save_ram[address >> 1] = value;
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

typedef struct bp_def {
	struct bp_def * next;
	uint32_t address;
	uint32_t index;
} bp_def;

bp_def * breakpoints = NULL;
uint32_t bp_index = 0;

bp_def ** find_breakpoint(bp_def ** cur, uint32_t address)
{
	while (*cur) {
		if ((*cur)->address == address) {
			break;
		}
		cur = &((*cur)->next);
	}
	return cur;
}

bp_def ** find_breakpoint_idx(bp_def ** cur, uint32_t index)
{
	while (*cur) {
		if ((*cur)->index == index) {
			break;
		}
		cur = &((*cur)->next);
	}
	return cur;
}

char * find_param(char * buf)
{
	for (; *buf; buf++) {
		if (*buf == ' ') {
			if (*(buf+1)) {
				return buf+1;
			}
		}
	}
	return NULL;
}

void strip_nl(char * buf)
{
	for(; *buf; buf++) {
		if (*buf == '\n') {
			*buf = 0;
			return;
		}
	}
}

m68k_context * debugger(m68k_context * context, uint32_t address)
{
	static char last_cmd[1024];
	char input_buf[1024];
	static uint32_t branch_t;
	static uint32_t branch_f;
	m68kinst inst;
	//probably not necessary, but let's play it safe
	address &= 0xFFFFFF;
	if (address == branch_t) {
		bp_def ** f_bp = find_breakpoint(&breakpoints, branch_f);
		if (!*f_bp) {
			remove_breakpoint(context, branch_f);
		}
		branch_t = branch_f = 0;
	} else if(address == branch_f) {
		bp_def ** t_bp = find_breakpoint(&breakpoints, branch_t);
		if (!*t_bp) {
			remove_breakpoint(context, branch_t);
		}
		branch_t = branch_f = 0;
	}
	//Check if this is a user set breakpoint, or just a temporary one
	bp_def ** this_bp = find_breakpoint(&breakpoints, address);
	if (*this_bp) {
		printf("Breakpoint %d hit\n", (*this_bp)->index);
	} else {
		remove_breakpoint(context, address);
	}
	uint16_t * pc;
	if (address < 0x400000) {
		pc = cart + address/2;
	} else if(address > 0xE00000) {
		pc = ram + (address & 0xFFFF)/2;
	} else {
		fprintf(stderr, "Entered debugger at address %X\n", address);
		exit(1);
	}
	uint16_t * after_pc = m68k_decode(pc, &inst, address);
	m68k_disasm(&inst, input_buf);
	printf("%X: %s\n", address, input_buf);
	uint32_t after = address + (after_pc-pc)*2;
	int debugging = 1;
	while (debugging) {
		fputs(">", stdout);
		if (!fgets(input_buf, sizeof(input_buf), stdin)) {
			fputs("fgets failed", stderr);
			break;
		}
		strip_nl(input_buf);
		//hitting enter repeats last command
		if (input_buf[0]) {
			strcpy(last_cmd, input_buf);
		} else {
			strcpy(input_buf, last_cmd);
		}
		char * param;
		char format[8];
		uint32_t value;
		bp_def * new_bp;
		switch(input_buf[0])
		{
			case 'c':
				puts("Continuing");
				debugging = 0;
				break;
			case 'b':
				param = find_param(input_buf);
				if (!param) {
					fputs("b command requires a parameter\n", stderr);
					break;
				}
				value = strtol(param, NULL, 16);
				insert_breakpoint(context, value, (uint8_t *)debugger);
				new_bp = malloc(sizeof(bp_def));
				new_bp->next = breakpoints;
				new_bp->address = value;
				new_bp->index = bp_index++;
				breakpoints = new_bp;
				printf("Breakpoint %d set at %X\n", new_bp->index, value);
				break;
			case 'a':
				param = find_param(input_buf);
				if (!param) {
					fputs("a command requires a parameter\n", stderr);
					break;
				}
				value = strtol(param, NULL, 16);
				insert_breakpoint(context, value, (uint8_t *)debugger);
				debugging = 0;
				break;
			case 'd':
				param = find_param(input_buf);
				if (!param) {
					fputs("b command requires a parameter\n", stderr);
					break;
				}
				value = atoi(param);
				this_bp = find_breakpoint_idx(&breakpoints, value);
				if (!*this_bp) {
					fprintf(stderr, "Breakpoint %d does not exist\n", value);
					break;
				}
				new_bp = *this_bp;
				*this_bp = (*this_bp)->next;
				free(new_bp);
				break;
			case 'p':
				strcpy(format, "%s: %d\n");
				if (input_buf[1] == '/') {
					switch (input_buf[2])
					{
					case 'x':
					case 'X':
					case 'd':
					case 'c':
						format[5] = input_buf[2];
						break;
					default:
						fprintf(stderr, "Unrecognized format character: %c\n", input_buf[2]);
					}
				}
				param = find_param(input_buf);
				if (!param) {
					fputs("p command requires a parameter\n", stderr);
					break;
				}
				if (param[0] == 'd' && param[1] >= '0' && param[1] <= '7') {
					value = context->dregs[param[1]-'0'];
				} else if (param[0] == 'a' && param[1] >= '0' && param[1] <= '7') {
					value = context->aregs[param[1]-'0'];
				} else if (param[0] == 'S' && param[1] == 'R') {
					value = (context->status << 8);
					for (int flag = 0; flag < 5; flag++) {
						value |= context->flags[flag] << (4-flag);
					}
				} else if(param[0] == 'c') {
					value = context->current_cycle;
				} else if (param[0] == '0' && param[1] == 'x') {
					uint32_t p_addr = strtol(param+2, NULL, 16);
					value = read_dma_value(p_addr/2);
				} else {
					fprintf(stderr, "Unrecognized parameter to p: %s\n", param);
					break;
				}
				printf(format, param, value);
				break;
			case 'n':
				//TODO: Deal with jmp, dbcc, rtr and rte
				if (inst.op == M68K_RTS) {
					after = (read_dma_value(context->aregs[7]/2) << 16) | read_dma_value(context->aregs[7]/2 + 1);
				} else if(inst.op == M68K_BCC && inst.extra.cond != COND_FALSE) {
					if (inst.extra.cond = COND_TRUE) {
						after = inst.address + 2 + inst.src.params.immed;
					} else {
						branch_f = after;
						branch_t = inst.address + 2 + inst.src.params.immed;
						insert_breakpoint(context, branch_t, (uint8_t *)debugger);
					}
				}
				insert_breakpoint(context, after, (uint8_t *)debugger);
				debugging = 0;
				break;
			case 'v': {
				genesis_context * gen = context->system;
				//VDP debug commands
				switch(input_buf[1])
				{
				case 's':
					vdp_print_sprite_table(gen->vdp);
					break;
				case 'r':
					vdp_print_reg_explain(gen->vdp);
					break;
				}
				break;
			}
			case 'q':
				puts("Quitting");
				exit(0);
				break;
			default:
				fprintf(stderr, "Unrecognized debugger command %s\n", input_buf);
				break;
		}
	}
	return context;
}

#define ROM_END   0x1A4
#define RAM_ID    0x1B0
#define RAM_FLAGS 0x1B2
#define RAM_START 0x1B4
#define RAM_END   0x1B8
#define MAX_MAP_CHUNKS (4+7+1)
#define RAM_FLAG_MASK 0x1800

const memmap_chunk static_map[] = {
		{0,        0x400000,  0xFFFFFF, 0, MMAP_READ,                          cart,
		           NULL,          NULL,         NULL,            NULL},
		{0xE00000, 0x1000000, 0xFFFF,   0, MMAP_READ | MMAP_WRITE | MMAP_CODE, ram, 
		           NULL,          NULL,         NULL,            NULL},
		{0xC00000, 0xE00000,  0x1FFFFF, 0, 0,                                  NULL,
		           (read_16_fun)vdp_port_read,  (write_16_fun)vdp_port_write,
		           (read_8_fun)vdp_port_read_b, (write_8_fun)vdp_port_write_b},
		{0xA00000, 0xA12000,  0x1FFFF,  0, 0,                                  NULL,
		           (read_16_fun)io_read_w,      (write_16_fun)io_write_w,
		           (read_8_fun)io_read,         (write_8_fun)io_write}
	};

char * sram_filename;
genesis_context * genesis;
void save_sram()
{
	FILE * f = fopen(sram_filename, "wb");
	if (!f) {
		fprintf(stderr, "Failed to open SRAM file %s for writing\n", sram_filename);
		return;
	}
	uint32_t size = genesis->save_ram_mask+1;
	if (genesis->save_flags != RAM_FLAG_BOTH) {
		size/= 2;
	}
	fwrite(genesis->save_ram, 1, size, f);
	fclose(f);
	printf("Saved SRAM to %s\n", sram_filename);
}

void init_run_cpu(genesis_context * gen, int debug, FILE * address_log)
{
	m68k_context context;
	x86_68k_options opts;
	gen->m68k = &context;
	memmap_chunk memmap[MAX_MAP_CHUNKS];
	uint32_t num_chunks;
	void * initial_mapped = NULL;
	gen->save_ram = NULL;
	//TODO: Handle carts larger than 4MB
	//TODO: Handle non-standard mappers
	uint32_t size;
	if ((cart[RAM_ID/2] & 0xFF) == 'A' && (cart[RAM_ID/2] >> 8) == 'R') {
		//Cart has save RAM
		uint32_t rom_end = ((cart[ROM_END/2] << 16) | cart[ROM_END/2+1]) + 1;
		uint32_t ram_start = (cart[RAM_START/2] << 16) | cart[RAM_START/2+1];
		uint32_t ram_end = (cart[RAM_END/2] << 16) | cart[RAM_END/2+1];
		uint16_t ram_flags = cart[RAM_FLAGS/2];
		gen->save_flags = ram_flags & RAM_FLAG_MASK;
		memset(memmap, 0, sizeof(memmap_chunk)*2);
		if (ram_start >= rom_end) {
			memmap[0].end = rom_end;
			memmap[0].mask = 0xFFFFFF;
			memmap[0].flags = MMAP_READ;
			memmap[0].buffer = cart;
			
			ram_start &= 0xFFFFFE;
			ram_end |= 1;
			memmap[1].start = ram_start;
			gen->save_ram_mask = memmap[1].mask = ram_end-ram_start;
			ram_end += 1;
			memmap[1].end = ram_end;
			memmap[1].flags = MMAP_READ | MMAP_WRITE;
			size = ram_end-ram_start;
			if ((ram_flags & RAM_FLAG_MASK) == RAM_FLAG_ODD) {
				memmap[1].flags |= MMAP_ONLY_ODD;
				size /= 2;
			} else if((ram_flags & RAM_FLAG_MASK) == RAM_FLAG_EVEN) {
				memmap[1].flags |= MMAP_ONLY_EVEN;
				size /= 2;
			}
			memmap[1].buffer = gen->save_ram = malloc(size);
			
			memcpy(memmap+2, static_map+1, sizeof(static_map)-sizeof(static_map[0]));
			num_chunks = sizeof(static_map)/sizeof(memmap_chunk)+1;
		} else {
			//Assume the standard Sega mapper for now
			memmap[0].end = 0x200000;
			memmap[0].mask = 0xFFFFFF;
			memmap[0].flags = MMAP_READ;
			memmap[0].buffer = cart;
			
			memmap[1].start = 0x200000;
			memmap[1].end = 0x400000;
			memmap[1].mask = 0x1FFFFF;
			ram_start &= 0xFFFFFE;
			ram_end |= 1;
			gen->save_ram_mask = ram_end-ram_start;
			memmap[1].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_FUNC_NULL;
			memmap[1].ptr_index = 2;
			memmap[1].read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[2] == NULL
			memmap[1].read_8 = (read_8_fun)read_sram_b;
			memmap[1].write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
			memmap[1].write_8 = (write_8_fun)write_sram_area_b;
			memcpy(memmap+2, static_map+1, sizeof(static_map)-sizeof(static_map[0]));
			num_chunks = sizeof(static_map)/sizeof(memmap_chunk)+1;
			memset(memmap+num_chunks, 0, sizeof(memmap[num_chunks]));
			memmap[num_chunks].start = 0xA13000;
			memmap[num_chunks].end = 0xA13100;
			memmap[num_chunks].mask = 0xFF;
			memmap[num_chunks].write_16 = (write_16_fun)write_bank_reg_w;
			memmap[num_chunks].write_8 = (write_8_fun)write_bank_reg_b; 
			num_chunks++;
			ram_end++;
			size = ram_end-ram_start;
			if ((ram_flags & RAM_FLAG_MASK) != RAM_FLAG_BOTH) {
				size /= 2;
			}
			gen->save_ram = malloc(size);
			memmap[1].buffer = initial_mapped = cart + 0x200000/2;
		}
	} else {
		memcpy(memmap, static_map, sizeof(static_map));
		num_chunks = sizeof(static_map)/sizeof(memmap_chunk);
	}
	if (gen->save_ram) {
		memset(gen->save_ram, 0, size);
		FILE * f = fopen(sram_filename, "rb");
		if (f) {
			uint32_t read = fread(gen->save_ram, 1, size, f);
			fclose(f);
			if (read > 0) {
				printf("Loaded SRAM from %s\n", sram_filename);
			}
		}
		atexit(save_sram);
	}
	init_x86_68k_opts(&opts, memmap, num_chunks);
	opts.address_log = address_log;
	init_68k_context(&context, opts.native_code_map, &opts);
	
	context.video_context = gen->vdp;
	context.system = gen;
	//cartridge ROM
	context.mem_pointers[0] = cart;
	context.target_cycle = context.sync_cycle = mclks_per_frame/MCLKS_PER_68K;
	//work RAM
	context.mem_pointers[1] = ram;
	//save RAM/map
	context.mem_pointers[2] = initial_mapped;
	context.mem_pointers[3] = (uint16_t *)gen->save_ram;
	uint32_t address;
	address = cart[2] << 16 | cart[3];
	translate_m68k_stream(address, &context);
	if (debug) {
		insert_breakpoint(&context, address, (uint8_t *)debugger);
	}
	m68k_reset(&context);
}

char title[64];

#define TITLE_START 0x150
#define TITLE_END (TITLE_START+48)

void update_title()
{
	uint16_t *last = cart + TITLE_END/2 - 1;
	while(last > cart + TITLE_START/2 && *last == 0x2020)
	{
		last--;
	}
	uint16_t *start = cart + TITLE_START/2;
	char *cur = title;
	char last_char = ' ';
	for (; start != last; start++)
	{
		if ((last_char != ' ' || (*start >> 8) != ' ') && (*start >> 8) < 0x80) {
			*(cur++) = *start >> 8;
			last_char = *start >> 8;
		}
		if (last_char != ' ' || (*start & 0xFF) != ' ' && (*start & 0xFF) < 0x80) {
			*(cur++) = *start;
			last_char = *start & 0xFF;
		}
	}
	*(cur++) = *start >> 8;
	if ((*start & 0xFF) != ' ') {
		*(cur++) = *start;
	}
	strcpy(cur, " - BlastEm");
}

#define REGION_START 0x1F0

int detect_specific_region(char region)
{
	return (cart[REGION_START/2] & 0xFF) == region || (cart[REGION_START/2] >> 8) == region || (cart[REGION_START/2+1] & 0xFF) == region;
}

void detect_region()
{
	if (detect_specific_region('U')|| detect_specific_region('B') || detect_specific_region('4')) {
		version_reg = NO_DISK | USA;
	} else if (detect_specific_region('J')) {
		version_reg = NO_DISK | JAP;
	} else if (detect_specific_region('E') || detect_specific_region('A')) {
		version_reg = NO_DISK | EUR;
	}
}

#define PSG_CLKS_NTSC (3579545/16)
#define PSG_CLKS_PAL (3546893/16)

int main(int argc, char ** argv)
{
	if (argc < 2) {
		fputs("Usage: blastem FILENAME\n", stderr);
		return 1;
	}
	if(!load_rom(argv[1])) {
		fprintf(stderr, "Failed to open %s for reading\n", argv[1]);
		return 1;
	}
	detect_region();
	int width = -1;
	int height = -1;
	int debug = 0;
	FILE *address_log = NULL;
	for (int i = 2; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch(argv[i][1]) {
			case 'd':
				debug = 1;
				break;
			case 'f':
				frame_limit = 1;
				break;
			case 'l':
				address_log = fopen("address.log", "w");
				break;
			case 'v':
				headless = 1;
				break;
			case 'n':
				z80_enabled = 0;
				break;
			case 'r':
				i++;
				if (i >= argc) {
					fputs("-r must be followed by region (J, U or E)\n", stderr);
					return 1;
				}
				switch (argv[i][0])
				{
				case 'j':
				case 'J':
					version_reg = NO_DISK | JAP;
					break;
				case 'u':
				case 'U':
					version_reg = NO_DISK | USA;
					break;
				case 'e':
				case 'E':
					version_reg = NO_DISK | EUR;
					break;
				default:
					fprintf(stderr, "'%c' is not a valid region character for the -r option\n", argv[i][0]);
					return 1;
				}
				break;
			default:
				fprintf(stderr, "Unrecognized switch %s\n", argv[i]);
				return 1;
			}
		} else if (width < 0) {
			width = atoi(argv[i]);
		} else if (height < 0) {
			height = atoi(argv[i]);
		}
	}
	update_title();
	width = width < 320 ? 320 : width;
	height = height < 240 ? (width/320) * 240 : height;
	uint32_t fps = 60;
	if (version_reg & 0x40) {
		mclks_per_frame = MCLKS_LINE * LINES_PAL;
		fps = 50;
	}
	if (!headless) {
		render_init(width, height, title, fps);
	}
	vdp_context v_context;
	
	init_vdp_context(&v_context);
	
	ym2612_context y_context;
	ym_init(&y_context);
	
	psg_context p_context;
	psg_init(&p_context, render_sample_rate(), fps == 60 ? PSG_CLKS_NTSC : PSG_CLKS_PAL, render_audio_buffer());
	
	z80_context z_context;
	x86_z80_options z_opts;
	init_x86_z80_opts(&z_opts);
	init_z80_context(&z_context, &z_opts);

	genesis_context gen;	

	z_context.system = &gen;
	z_context.mem_pointers[0] = z80_ram;
	z_context.sync_cycle = z_context.target_cycle = mclks_per_frame/MCLKS_PER_Z80;
	z_context.int_cycle = CYCLE_NEVER;
	z_context.mem_pointers[1] = z_context.mem_pointers[2] = (uint8_t *)cart;
	
	gen.z80 = &z_context;
	gen.vdp = &v_context;
	gen.ym = &y_context;
	gen.psg = &p_context;
	genesis = &gen;
	
	int fname_size = strlen(argv[1]);
	sram_filename = malloc(fname_size+6);
	memcpy(sram_filename, argv[1], fname_size);
	int i;
	for (i = fname_size-1; fname_size >= 0; --i) {
		if (sram_filename[i] == '.') {
			strcpy(sram_filename + i + 1, "sram");
			break;
		}
	}
	if (i < 0) {
		strcpy(sram_filename + fname_size, ".sram");
	}
	
	init_run_cpu(&gen, debug, address_log);
	return 0;
}
