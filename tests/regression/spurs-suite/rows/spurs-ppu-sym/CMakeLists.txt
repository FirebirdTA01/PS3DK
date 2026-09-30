# PPU symbols from the SPU: a SPURS task reaches PPU globals through
# CELL_SPURS_PPU_SYM, embedded with spu-elf-to-ppu-obj (PPU_OBJECT)
suite_ppu(spurs-ppu-sym ppu_sym.cpp)
suite_task_ppu_object(spurs-ppu-sym ppu_sym.c)
suite_self(spurs-ppu-sym)
