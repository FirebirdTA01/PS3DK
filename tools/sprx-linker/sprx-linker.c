/*
 * sprx-linker — post-link ELF fixups for CellOS Lv-2 user binaries.
 *
 * Fork of PSL1GHT's sprxlinker (MIT). Preserves the tool's two
 * essential roles: filling in per-stub import counts in .lib.stub,
 * and packing the compact (func_lo32, rtoc_lo32) pair into .opd
 * descriptors for the Lv-2 loader.
 *
 * Differences from upstream:
 *   - Accepts both the PSL1GHT-legacy 40-byte (.sys_proc_prx_param
 *     size = 0x28) and the spec-correct 64-byte (size = 0x40) prx
 *     param layouts. The check is narrowed to the magic word only
 *     (0x1b434cec at offset 4); the size field is no longer part of
 *     the identity check.
 *   - Skips the EI_OSABI rewrite: the PPU toolchain's binutils
 *     emits the correct byte directly when .sys_proc_prx_param is
 *     present (see patches/ppu/binutils-2.42/0001-*.patch).
 *   - Skips the .opd packing step when the section already uses the
 *     spec-correct 8-byte descriptor stride. Only 24-byte ELFv1
 *     descriptors need the compat pack.
 *
 * The ABI contract is documented in docs/abi/cellos-lv2-abi-spec.md.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <libelf.h>

#if defined(_WIN32)
#define OFLAGS		(O_RDWR|O_BINARY)
#else
#define OFLAGS		(O_RDWR)
#endif

#ifdef __BIG_ENDIAN__
#define BE16(num) (num)
#define BE32(num) (num)
#define BE64(num) (num)
#else
#define BE16(num) ((uint16_t)(((uint16_t)((num) << 8)) | (uint8_t)((num) >> 8)))
#define BE32(num) ((uint32_t)((uint32_t)BE16(num) << 16) | BE16(num >> 16))
#define BE64(num) ((uint64_t)((uint64_t)BE32(num) << 32ULL) | BE32(num >> 32ULL))
#endif

typedef struct _stub
{
	uint32_t header1;
	uint16_t header2;
	uint16_t imports;
	uint32_t zero1;
	uint32_t zero2;
	uint32_t name;
	uint32_t fnid;
	uint32_t stub;
	uint32_t zero3;
	uint32_t zero4;
	uint32_t zero5;
	uint32_t zero6;
} __attribute__((packed)) Stub;

typedef struct _opd64
{
	uint64_t func;
	uint64_t rtoc;
	uint64_t data;
} __attribute__((packed)) Opd24;

/* prx_magic: the 4-byte little-endian-on-disk = big-endian-as-integer
   marker at offset 4 of .sys_proc_prx_param. Size word (offset 0)
   intentionally not compared — both 0x28 and 0x40 are accepted. */
static const unsigned char PRX_MAGIC[4] = { 0x1b, 0x43, 0x4c, 0xec };

static Elf_Scn *getSection(Elf *elf, const char *name)
{
	Elf64_Ehdr *ehdr = elf64_getehdr(elf);
	if(!ehdr) return NULL;

	Elf_Data *data = elf_getdata(elf_getscn(elf, ehdr->e_shstrndx), NULL);
	if(!data) return NULL;

	for(Elf_Scn *scn = NULL; (scn = elf_nextscn(elf, scn));) {
		Elf64_Shdr *shdr = elf64_getshdr(scn);
		if(shdr && !strcmp(name, (const char *)data->d_buf + shdr->sh_name))
			return scn;
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	if(argc < 2) {
		printf("Usage: %s [elf path]\n", argv[0]);
		return 0;
	}

	int fd = open(argv[1], OFLAGS);
	if(fd < 0) {
		fprintf(stderr, "sprx-linker: unable to open elf file: %s\n", argv[1]);
		return 1;
	}

	elf_version(EV_CURRENT);

	Elf *elf = elf_begin(fd, ELF_C_READ, NULL);
	if(!elf) {
		fprintf(stderr, "sprx-linker: libelf could not read elf file: %s\n",
			elf_errmsg(elf_errno()));
		close(fd);
		return 1;
	}

	/* Verify .sys_proc_prx_param identity. We check only the magic
	   word; the size word may be 0x28 (legacy PSL1GHT) or 0x40
	   (spec). */
	Elf_Scn *prx = getSection(elf, ".sys_proc_prx_param");
	if(!prx) {
		fprintf(stderr, "sprx-linker: elf has no .sys_proc_prx_param section.\n");
		elf_end(elf);
		close(fd);
		return 1;
	}
	Elf_Data *prx_data = elf_getdata(prx, NULL);
	if(!prx_data || prx_data->d_size < 8
	   || memcmp((unsigned char *)prx_data->d_buf + 4, PRX_MAGIC, sizeof(PRX_MAGIC))) {
		fprintf(stderr,
		    "sprx-linker: .sys_proc_prx_param magic mismatch "
		    "(expected 0x1b434cec at offset 4).\n");
		elf_end(elf);
		close(fd);
		return 1;
	}

	/* Fix up per-stub import counts in .lib.stub. Each Stub covers a
	   contiguous run of FNID entries; we count the run by finding the
	   next FNID address and dividing by 4 (each FNID is 4 bytes). */
	Elf_Scn *stubsection = getSection(elf, ".lib.stub");
	if(stubsection) {
		Elf_Data *stubdata = elf_getdata(stubsection, NULL);
		Elf64_Shdr *stubshdr = elf64_getshdr(stubsection);
		Stub *stubbase = (Stub *)stubdata->d_buf;
		size_t stubcount = stubdata->d_size / sizeof(Stub);
		Elf_Scn *fnidsection = getSection(elf, ".rodata.sceFNID");
		if(fnidsection) {
			Elf64_Shdr *fnidshdr = elf64_getshdr(fnidsection);
			for(Stub *stub = stubbase; stub < stubbase + stubcount; stub++) {
				uint32_t fnid = BE32(stub->fnid);
				uint32_t end = fnidshdr->sh_addr + fnidshdr->sh_size;
				for(Stub *substub = stubbase; substub < (stubbase + stubcount); substub++) {
					if(stub == substub) continue;
					uint32_t newfnid = BE32(substub->fnid);
					if(newfnid >= fnid && newfnid < end) end = newfnid;
				}
				uint16_t fnidcount = (end - fnid) / 4;
				if(BE16(stub->imports) != fnidcount) {
					lseek(fd, stubshdr->sh_offset
					    + (stub - stubbase) * sizeof(Stub)
					    + offsetof(Stub, imports), SEEK_SET);
					uint16_t be_fnidcount = BE16(fnidcount);
					if(write(fd, &be_fnidcount, sizeof(be_fnidcount))
					    != (ssize_t)sizeof(be_fnidcount)) {
						fprintf(stderr,
						    "sprx-linker: write stub imports "
						    "failed at %s:%d\n",
						    __FILE__, __LINE__);
					}
				}
			}
		}
	}

	/* .opd compat packing. Only apply to 24-byte ELFv1 descriptors —
	   the spec-correct 8-byte descriptors need no post-processing. */
	Elf_Scn *opdsection = getSection(elf, ".opd");
	if(opdsection) {
		Elf_Data *opddata = elf_getdata(opdsection, NULL);
		Elf64_Shdr *opdshdr = elf64_getshdr(opdsection);
		if(opddata && opddata->d_size % sizeof(Opd24) == 0
		   && opddata->d_size >= sizeof(Opd24)) {
			Opd24 *opdbase = (Opd24 *)opddata->d_buf;
			size_t opdcount = opddata->d_size / sizeof(Opd24);
			for(Opd24 *opd = opdbase; opd < (opdbase + opdcount); opd++) {
				opd->data = BE64(((BE64(opd->func) << 32ULL)
				                   | (BE64(opd->rtoc) & 0xffffffffULL)));
				lseek(fd, opdshdr->sh_offset
				    + (opd - opdbase) * sizeof(Opd24), SEEK_SET);
				if(write(fd, opd, sizeof(Opd24)) != (ssize_t)sizeof(Opd24)) {
					fprintf(stderr,
					    "sprx-linker: write opd failed at %s:%d\n",
					    __FILE__, __LINE__);
				}
			}
		}
		/* else: 8-byte entries or empty — leave alone. */
	}

	elf_end(elf);
	close(fd);
	return 0;
}
