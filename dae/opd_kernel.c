/**
 * \file opd_kernel.c
 * Copyright 2002 OProfile authors
 * Read the file COPYING
 *
 * \author John Levon <moz@compsoc.man.ac.uk>
 * \author Philippe Elie <phil_el@wanadoo.fr>
 */

#include "opd_proc.h"

#include "op_fileio.h"
 
extern int verbose;
extern unsigned long opd_stats[];
 
extern struct opd_image * kernel_image;

/* kernel and module support */
u32 kernel_start;
static u32 kernel_end;
static struct opd_module opd_modules[OPD_MAX_MODULES];
static unsigned int nr_modules=0;

/**
 * opd_read_system_map - parse System.map file
 * @param filename  file name of System.map
 *
 * Parse the kernel's System.map file. If the filename is
 * passed as "", a warning is produced and the function returns.
 *
 * If the file is parsed correctly, the global variables
 * kernel_start and kernel_end are set to the correct values for the
 * text section of the mainline kernel else kernel_start is set to
 * KERNEL_VMA_OFFSET and kernel_end to infinite
 *
 * Note that kernel modules will have EIP values above the value of
 * kernel_end.
 */
void opd_read_system_map(const char *filename)
{
	FILE *fp;
	char *line;
	char *cp;

	fp = op_open_file(filename, "r");

	while (1) {
		line = op_get_line(fp);
		if (streq(line, "")) {
			free(line);
			break;
		} else {
			if (strlen(line) < 11) {
				free(line);
				continue;
			}
			cp = line+11;
			if (streq("_text", cp))
				sscanf(line, "%x", &kernel_start);
			else if (streq("_end", cp))
				sscanf(line, "%x", &kernel_end);
			free(line);
		}
	}

	if (!kernel_start)
		kernel_start = KERNEL_VMA_OFFSET;
	if (!kernel_end)
		kernel_end = (u32)-1;

	op_close_file(fp);
}

/**
 * opd_clear_module_info - clear kernel module information
 *
 * Clear and free all kernel module information and reset
 * values.
 */
void opd_clear_module_info(void)
{
	int i;

	for (i=0; i < OPD_MAX_MODULES; i++) {
		if (opd_modules[i].name)
			free(opd_modules[i].name);
		opd_modules[i].name = NULL;
		opd_modules[i].start = 0;
		opd_modules[i].end = 0;
	}
}

/**
 * opd_get_module - get module structure
 * @param name  name of module image
 *
 * Find the module structure for module image @name.
 * If it could not be found, add the module to
 * the global module structure.
 *
 * If an existing module is found, @name is free()d.
 * Otherwise it must be freed when the module structure
 * is removed (i.e. in opd_clear_module_info()).
 */
static struct opd_module *opd_get_module(char *name)
{
	int i;

	for (i=0; i < OPD_MAX_MODULES; i++) {
		if (opd_modules[i].name && bstreq(name, opd_modules[i].name)) {
			/* free this copy */
			free(name);
			return &opd_modules[i];
		}
	}

	opd_modules[nr_modules].name = name;
	opd_modules[nr_modules].image = NULL;
	opd_modules[nr_modules].start = 0;
	opd_modules[nr_modules].end = 0;
	nr_modules++;
	if (nr_modules == OPD_MAX_MODULES) {
		fprintf(stderr, "Exceeded %u kernel modules !\n", OPD_MAX_MODULES);
		exit(EXIT_FAILURE);
	}

	return &opd_modules[nr_modules-1];
}

/**
 * opd_get_module_info - parse mapping information for kernel modules
 *
 * Parse the file /proc/ksyms to read in mapping information for
 * all kernel modules. The modutils package adds special symbols
 * to this file which allows determination of the module image
 * and mapping addresses of the form :
 *
 * __insmod_modulename_Oobjectfile_Mmtime_Vversion
 * __insmod_modulename_Ssectionname_Llength
 *
 * Currently the image file "objectfile" is stored, and details of
 * ".text" sections.
 *
 * There is no query_module API that allow to get directly the pathname
 * of a module so we need to parse all the /proc/ksyms.
 */
static void opd_get_module_info(void)
{
	char *line;
	char *cp, *cp2, *cp3;
	FILE *fp;
	struct opd_module *mod;
	char *modname;
	char *filename;

	nr_modules=0;

	fp = op_try_open_file("/proc/ksyms", "r");

	if (!fp) {	
		printf("oprofiled: /proc/ksyms not readable, can't process module samples.\n");
		return;
	}

	while (1) {
		line = op_get_line(fp);
		if (streq("", line) && !feof(fp)) {
			free(line);
			continue;
		} else if (streq("",line))
			goto failure;

		if (strlen(line) < 9) {
			printf("oprofiled: corrupt /proc/ksyms line \"%s\"\n", line);
			goto failure;
		}

		if (strncmp("__insmod_", line+9, 9)) {
			free(line);
			continue;
		}

		cp = line + 18;
		cp2 = cp;
		while ((*cp2) && !streqn("_S", cp2+1, 2) && !streqn("_O", cp2+1, 2))
			cp2++;

		if (!*cp2) {
			printf("oprofiled: corrupt /proc/ksyms line \"%s\"\n", line);
			goto failure;
		}
	
		cp2++;
		/* freed by opd_clear_module_info() or opd_get_module() */
		modname = xmalloc((size_t)((cp2-cp) + 1));
		strncpy(modname, cp, (size_t)((cp2-cp)));
		modname[cp2-cp] = '\0';

		mod = opd_get_module(modname);

		switch (*(++cp2)) {
			case 'O':
				/* get filename */
				cp2++;
				cp3 = cp2;

				while ((*cp3) && !streqn("_M", cp3+1, 2))
					cp3++;

				if (!*cp3) {
					free(line);
					continue;
				}
				
				cp3++;
				filename = xmalloc((size_t)(cp3 - cp2 + 1));
				strncpy(filename, cp2, (size_t)(cp3 - cp2));
				filename[cp3-cp2] = '\0';

				mod->image = opd_get_image(filename, -1, NULL, 1);
				free(filename);
				break;

			case 'S':
				/* get extent of .text section */
				cp2++;
				if (strncmp(".text_L", cp2, 7)) {
					free(line);
					continue;
				}

				cp2 += 7;
				sscanf(line,"%x", &mod->start);
				sscanf(cp2,"%u", &mod->end);
				mod->end += mod->start;
				break;
		}

		free(line);
	}

failure:
	free(line);
	op_close_file(fp);
}

/**
 * opd_enter_invalid_module - create a negative module entry
 * @param name  name of module
 * @param info  module info
 */
static void opd_enter_invalid_module(char const * name, struct module_info * info)
{
	opd_modules[nr_modules].name = xstrdup(name);
	opd_modules[nr_modules].image = NULL;
	opd_modules[nr_modules].start = info->addr;
	opd_modules[nr_modules].end = info->addr + info->size;
	nr_modules++;
	if (nr_modules == OPD_MAX_MODULES) {
		fprintf(stderr, "Exceeded %u kernel modules !\n", OPD_MAX_MODULES);
		exit(EXIT_FAILURE);
	}
}

/**
 * opd_drop_module_sample - drop a module sample efficiently
 * @param eip  eip of sample
 */
static void opd_drop_module_sample(u32 eip)
{
	char * module_names;
	char * name;
	size_t size = 1024;
	size_t ret;
	uint nr_mods;
	uint mod = 0;
	
	module_names = xmalloc(size);
	while (query_module(NULL, QM_MODULES, module_names, size, &ret)) {
		if (errno != ENOSPC) {
			verbprintf("query_module failed: %s\n", strerror(errno)); 
			return;
		}
		size = ret;
		module_names = xrealloc(module_names, size);
	}

	nr_mods = ret;
	name = module_names;

	while (mod < nr_mods) {
		struct module_info info;
		if (!query_module(name, QM_INFO, &info, sizeof(info), &ret)) {
			if (eip >= info.addr && eip < info.addr + info.size) {
				verbprintf("Sample from unprofilable module %s\n", name);
				opd_enter_invalid_module(name, &info);
				goto out;
			}
		}
		mod++;
		name += strlen(name) + 1;
	}
out:
	if (module_names) free(module_names);
}

/**
 * opd_find_module_by_eip - find a module by its eip
 * @param eip  EIP value
 *
 * find in the modules container the module which
 * contain this @eip return %NULL if not found.
 * caller must check than the module image is valid
 */
static struct opd_module * opd_find_module_by_eip(u32 eip)
{
	uint i;
	for (i = 0; i < nr_modules; i++) {
		if (opd_modules[i].start && opd_modules[i].end &&
		    opd_modules[i].start <= eip && opd_modules[i].end > eip)
			return &opd_modules[i];
	}

	return NULL;
}

/**
 * opd_handle_module_sample - process a module sample
 * @param eip  EIP value
 * @param count  count value of sample
 *
 * Process a sample in module address space. The sample @eip
 * is matched against module information. If the search was
 * successful, the sample is output to the relevant file.
 *
 * Note that for modules and the kernel, the offset will be
 * wrong in the file, as it is not a file offset, but the offset
 * from the text section. This is fixed up in pp.
 *
 * If the sample could not be located in a module, it is treated
 * as a kernel sample.
 */
static void opd_handle_module_sample(u32 eip, u16 count)
{
	struct opd_module * module;

	module = opd_find_module_by_eip(eip);
	if (!module) {
		/* not found in known modules, re-read our info and retry */
		opd_clear_module_info();
		opd_get_module_info();

		module = opd_find_module_by_eip(eip);
	}

	if (module) {
		if (module->image != NULL) {
			opd_stats[OPD_MODULE]++;
			opd_put_image_sample(module->image, 
					     eip - module->start, count);
		} else {
			opd_stats[OPD_LOST_MODULE]++;
			verbprintf("No image for sampled module %s\n",
				   module->name);
		}
	} else {
		/* ok, we failed to place the sample even after re-reading
		 * /proc/ksyms. It's either a rogue sample, or from a module
		 * that didn't create symbols (like in some initrd setups).
		 * So we check with query_module() if we can place it in a
		 * symbol-less module, and if so create a negative entry for
		 * it, to quickly ignore future samples.
		 *
		 * Problem uncovered by Bob Montgomery <bobm@fc.hp.com>
		 */
		opd_stats[OPD_LOST_MODULE]++;
		opd_drop_module_sample(eip);
	}
}

/**
 * opd_handle_kernel_sample - process a kernel sample
 * @param eip  EIP value of sample
 * @param count  count value of sample
 *
 * Handle a sample in kernel address space or in a module. The sample is
 * output to the relevant image file.
 *
 * This function requires the global variable kernel_end to be known
 * through the System.map to handle module samples.
 */
void opd_handle_kernel_sample(u32 eip, u16 count)
{
	if (eip < kernel_end) {
		opd_stats[OPD_KERNEL]++;
		opd_put_image_sample(kernel_image, eip - kernel_start, count);
		return;
	}

	/* in a module */
	opd_handle_module_sample(eip, count);
}
