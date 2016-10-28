/*
 * Copyright(C) 2011-2016 Pedro H. Penna   <pedrohenriquepenna@gmail.com>
 *              2015-2016 Davidson Francis <davidsondfgl@gmail.com> 
 *
 * This file is part of Nanvix.
 * 
 * Nanvix is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Nanvix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Nanvix. If not, see <http://www.gnu.org/licenses/>.
 */

#include <nanvix/config.h>
#include <nanvix/const.h>
#include <nanvix/clock.h>
#include <nanvix/dev.h>
#include <nanvix/fs.h>
#include <nanvix/hal.h>
#include <nanvix/klib.h>
#include <nanvix/mm.h>
#include <nanvix/region.h>
#include <signal.h>
#include "mm.h"

/*============================================================================*
 *                             Kernel Page Pool                               *
 *============================================================================*/

/* Kernel pages. */
#define NR_KPAGES (KPOOL_SIZE/PAGE_SIZE) /* Number of kernel pages.  */
PRIVATE int kpages[NR_KPAGES] = { 0,  }; /* Reference count.         */

/**
 * @brief Allocates a kernel page.
 * 
 * @param clean Should the page be cleaned?
 * 
 * @returns Upon success, a pointer to a page is returned. Upon
 * failure, a NULL pointer is returned instead.
 */
PUBLIC void *getkpg(int clean)
{
	unsigned i; /* Loop index.  */
	void *kpg;  /* Kernel page. */
	
	/* Search for a free kernel page. */
	for (i = 0; i < NR_KPAGES; i++)
	{
		/* Found it. */
		if (kpages[i] == 0)
			goto found;
	}

	kprintf("mm: kernel page pool overflow");
	
	return (NULL);

found:

	/* Set page as used. */
	kpg = (void *)(KPOOL_VIRT + (i << PAGE_SHIFT));
	kpages[i]++;
	
	/* Clean page. */
	if (clean)
		kmemset(kpg, 0, PAGE_SIZE);
	
	return (kpg);
}

/**
 * @brief Releases kernel page.
 * 
 * @param kpg Kernel page to be released.
 */
PUBLIC void putkpg(void *kpg)
{
	unsigned i;
	
	i = ((addr_t)kpg - KPOOL_VIRT) >> PAGE_SHIFT;
	
	/* Double free. */
	if (--kpages[i] < 0)
		kpanic("mm: releasing kernel page twice");
}

/*============================================================================*
 *                             Page Frames Subsystem                          *
 *============================================================================*/

/**
 * @brief Number of page frames.
 */
#define NR_FRAMES (UMEM_SIZE/PAGE_SIZE)

/**
 * @brief Reference count for page frames.
 */
PRIVATE unsigned frames[NR_FRAMES] = {0, };

/**
 * @brief Converts a frame ID to a frame number.
 *
 * @param id ID of target page frame.
 *
 * @returns Frame number of target page frame.
 */
PRIVATE inline addr_t frame_id_to_addr(int id)
{
	return ((UBASE_PHYS >> PAGE_SHIFT) + id);
}

/**
 * @brief Converts a frame number to a frame ID.
 *
 * @param addr Frame number of target page frame
 *
 * @returns ID of target page frame.
 */
PRIVATE inline int frame_addr_to_id(addr_t addr)
{
	return (addr - (UBASE_PHYS >> PAGE_SHIFT));
}

/**
 * @brief Allocates a page frame.
 * 
 * @returns The page frame number upon success, and zero upon failure.
 */
PRIVATE addr_t frame_alloc(void)
{
	/* Search for a free frame. */
	for (int i = 0; i < NR_FRAMES; i++)
	{
		/* Found it. */
		if (frames[i] == 0)
		{
			frames[i] = 1;
			
			return (frame_id_to_addr(i));
		}
	}
	
	return (0);
}

/**
 * @brief Frees a page frame.
 *
 * @param addr Frame number of target page frame.
 */
PRIVATE inline void frame_free(addr_t addr)
{
	/* Double free? */
	if (frames[frame_addr_to_id(addr)]-- == 0)
		kpanic("mm: double free on page frame");
}

/**
 * @brief Increments the reference count of a page frame.
 *
 * @param i ID of target page frame.
 */
PRIVATE inline void frame_share(addr_t addr)
{
	frames[frame_addr_to_id(addr)]++;
}

/**
 * @brief Asserts if a page frame is begin shared.
 *
 * @param addr Frame number of target page frame.
 *
 * @returns Non zero if the page frame is being shared, and zero
 * otherwise.
 */
PRIVATE inline int frame_is_shared(addr_t addr)
{
	return (frames[frame_addr_to_id(addr)] > 1);
}

/*============================================================================*
 *                              Paging System                                 *
 *============================================================================*/

/**
 * @brief Gets a page directory entry.
 * 
 * @param proc Target process.
 * @param addr Target address.
 * 
 * @returns The requested page directory entry.
 */
PRIVATE inline struct pde *getpde(struct process *proc, addr_t addr)
{
	return (&proc->pgdir[PGTAB(addr)]);
}

/**
 * @brief Gets a page table entry of a process.
 * 
 * @param proc Target process.
 * @param addr Target address.
 * 
 * @returns The requested page table entry.
 */
PRIVATE inline struct pte *getpte(struct process *proc, addr_t addr)
{
	addr_t base;

	base = (getpde(proc, addr)->frame << PAGE_SHIFT) + KBASE_VIRT;

	return (&((struct pte *) base)[PG(addr)]);
}

/**
 * @brief Copies a page.
 * 
 * @brief pg1 Target page.
 * @brief pg2 Source page.
 * 
 * @returns Zero upon successful completion, and non-zero otherwise.
 * 
 * @note The source page is assumed to be in-core.
 */
PRIVATE int cpypg(struct pte *pg1, struct pte *pg2)
{
	addr_t addr;
	
	/* Allocate new user page. */
	if (!(addr = frame_alloc()))
		return (-1);
	
	/* Handcraft page table entry. */
	pte_copy(pg1, pg2);
	pg1->frame = addr;

	physcpy(pg1->frame << PAGE_SHIFT, pg2->frame << PAGE_SHIFT, PAGE_SIZE);
	
	return (0);
}

/**
 * @brief Allocates a user page.
 * 
 * @param addr     Address where the page resides.
 * @param writable Is the page writable?
 * 
 * @returns Zero upon successful completion, and non-zero otherwise.
 */
PRIVATE int allocupg(addr_t vaddr, int writable)
{
	addr_t paddr;   /* Page address.             */
	struct pte *pg; /* Working page table entry. */
	
	/* Failed to allocate page frame. */
	if (!(paddr = frame_alloc()))
		return (-1);

	vaddr &= PAGE_MASK;
	
	/* Allocate page. */
	pg = getpte(curr_proc, vaddr);
	pte_clear(pg);
	pte_present_set(pg, 1);
	pte_write_set(pg, writable);
	pte_user_set(pg, 1);
	pg->frame = paddr;
	tlb_flush();
	
	kmemset((void *)(vaddr), 0, PAGE_SIZE);
	
	return (0);
}

/**
 * @brief Reads a page from a file.
 * 
 * @param reg  Region where the page resides.
 * @param addr Address where the page should be loaded. 
 * 
 * @returns Zero upon successful completion, and non-zero upon failure.
 */
PRIVATE int readpg(struct region *reg, addr_t addr)
{
	char *p;             /* Read pointer.             */
	off_t off;           /* Block offset.             */
	ssize_t count;       /* Bytes read.               */
	struct inode *inode; /* File inode.               */
	struct pte *pg;      /* Working page table entry. */
	
	addr &= PAGE_MASK;
	
	/* Assign a user page. */
	if (allocupg(addr, reg->mode & MAY_WRITE))
		return (-1);
	
	/* Find page table entry. */
	pg = getpte(curr_proc, addr);
	
	/* Read page. */
	off = reg->file.off + (PG(addr) << PAGE_SHIFT);
	inode = reg->file.inode;
	p = (char *)(addr);
	count = file_read(inode, p, PAGE_SIZE, off);
	
	/* Failed to read page. */
	if (count < 0)
	{
		freeupg(pg);
		return (-1);
	}
	
	return (0);
}

/**
 * @brief Maps a page table into user address space.
 * 
 * @param proc  Process in which the page table should be mapped.
 * @param addr  Address where the page should be mapped.
 * @param pgtab Page table to map.
 */
PUBLIC void mappgtab(struct process *proc, addr_t addr, void *pgtab)
{
	struct pde *pde;
	
	pde = &proc->pgdir[PGTAB(addr)];
	
	/* Bad page table. */
	if (pde->present)
		kpanic("busy page table entry");
	
	/* Map kernel page. */
	pde->present = 1;
	pde->writable = 1;
	pde->user = 1;
	pde->frame = (ADDR(pgtab) - KBASE_VIRT) >> PAGE_SHIFT;
	
	/* Flush changes. */
	if (proc == curr_proc)
		tlb_flush();
}

/**
 * @brief Unmaps a page table from user address space.
 * 
 * @param proc Process in which the page table should be unmapped.
 * @param addr Address where the page should be unmapped.
 * 
 * @returns Zero upon success, and non zero otherwise.
 */
PUBLIC void umappgtab(struct process *proc, addr_t addr)
{
	struct pde *pde;
	
	pde = &proc->pgdir[PGTAB(addr)];
	
	/* Bad page table. */
	if (!(pde->present))
		kpanic("mm: unmap non-present page table");

	/* Unmap kernel page. */
	kmemset(pde, 0, sizeof(struct pde));
	
	/* Flush changes. */
	if (proc == curr_proc)
		tlb_flush();
}

/**
 * @brief Frees a user page.
 * 
 * @param pg Page to be freed.
 */
PUBLIC void freeupg(struct pte *pg)
{
	/* Do nothing. */
	if (pte_is_clear(pg))
		return;
	
	/* Check for demand page. */
	if (!pte_is_present(pg))
	{
		/* Demand page. */
		if (pte_is_fill(pg) || pte_is_zero(pg))
			goto done;

		kpanic("mm: freeing invalid user page");
	}
		
	frame_free(pg->frame);

done:
	pte_clear(pg);
	tlb_flush();
}

/**
 * @brief Marks a page.
 * 
 * @param pg   Page to be marked.
 * @param mark Mark.
 */
PUBLIC void markpg(struct pte *pg, int mark)
{
	/* Bad page. */
	if (pte_is_present(pg))
		kpanic("mm: demand fill on a present page");
	
	/* Mark page. */
	switch (mark)
	{
		/* Demand fill. */
		case PAGE_FILL:
			pte_fill_set(pg, 1);
			pte_zero_set(pg, 0);
			break;
		
		/* Demand zero. */
		case PAGE_ZERO:
			pte_fill_set(pg, 0);
			pte_zero_set(pg, 1);
			break;
	}
}

/**
 * @brief Enables copy-on-write on a page.
 *
 * @param pg Target page.
 */
PRIVATE void cow_enable(struct pte *pg)
{
	pg->cow = 1;
	pte_write_set(pg, 0);
}

/**
 * @brief Disables copy-on-write on a page.
 *
 * @param pg Target page.
 *
 * @returns Zero on success, and non zero otherwise.
 */
PRIVATE int cow_disable(struct pte *pg)
{
	/* Steal page. */
	if (frame_is_shared(pg->frame))
	{
		struct pte new_pg;

		/* Copy page. */
		if (cpypg(&new_pg, pg))
			return (-1);
		
		/* Unlink page. */
		frame_free(pg->frame);
		kmemcpy(pg, &new_pg, sizeof(struct pte));
	}

	pg->cow = 0;
	pte_write_set(pg, 1);

	return (0);
}

/**
 * @brief Asserts if copy-on-write is enabled on a page.
 *
 * @param pg Target page.
 *
 * @returns Non zero if copy-on-write is enabled, and zero otherwise.
 */
PRIVATE int cow_enabled(struct pte *pg)
{
	return ((pg->cow) && (!pte_is_write(pg)));
}

/**
 * @brief Links two pages.
 * 
 * @param upg1 Source page.
 * @param upg2 Target page.
 */
PUBLIC void linkupg(struct pte *upg1, struct pte *upg2)
{	
	/* Nothing to do. */
	if (pte_is_clear(upg1))
		return;

	/* Invalid. */
	if (!pte_is_present(upg1))
	{
		/* Demand page. */
		if (pte_is_fill(upg1) || pte_is_zero(upg1))
		{
			kmemcpy(upg2, upg1, sizeof(struct pte));
			return;
		}

		kpanic("linking invalid user page");
	}

	/* Set copy on write. */
	if (pte_is_write(upg1))
		cow_enable(upg1);

	frame_share(upg1->frame);
	
	kmemcpy(upg2, upg1, sizeof(struct pte));
}

/**
 * @brief Creates a page directory for a process.
 * 
 * @param proc Target process.
 * 
 * @returns Upon successful completion, zero is returned. Upon
 * failure, non-zero is returned instead.
 */
PUBLIC int crtpgdir(struct process *proc)
{
	void *kstack;             /* Kernel stack.     */
	struct pde *pgdir;        /* Page directory.   */
	struct intstack *s1, *s2; /* Interrupt stacks. */
	
	/* Get kernel page for page directory. */
	pgdir = getkpg(1);
	if (pgdir == NULL)
		goto err0;

	/* Get kernel page for kernel stack. */
	kstack = getkpg(0);
	if (kstack == NULL)
		goto err1;

	/* Build page directory. */
	pgdir[0] = curr_proc->pgdir[0];
	pgdir[PGTAB(KBASE_VIRT)] = curr_proc->pgdir[PGTAB(KBASE_VIRT)];
	pgdir[PGTAB(KPOOL_VIRT)] = curr_proc->pgdir[PGTAB(KPOOL_VIRT)];
	pgdir[PGTAB(INITRD_VIRT)] = curr_proc->pgdir[PGTAB(INITRD_VIRT)];
	
	/* Clone kernel stack. */
	kmemcpy(kstack, curr_proc->kstack, KSTACK_SIZE);
	
	/* Adjust stack pointers. */
	proc->kesp = (curr_proc->kesp -(dword_t)curr_proc->kstack)+(dword_t)kstack;
	if (KERNEL_RUNNING(curr_proc))
	{
		s1 = (struct intstack *) curr_proc->kesp;
		s2 = (struct intstack *) proc->kesp;	
		s2->ebp = (s1->ebp - (dword_t)curr_proc->kstack) + (dword_t)kstack;
	}
	/* Assign page directory. */
	proc->cr3 = ADDR(pgdir) - KBASE_VIRT;
	proc->pgdir = pgdir;
	proc->kstack = kstack;
	
	return (0);

err1:
	putkpg(pgdir);
err0:
	return (-1);
}

/**
 * @brief Destroys the page directory of a process.
 * 
 * @param proc Target process.
 * 
 * @note The current running process may not be the target process.
 */
PUBLIC void dstrypgdir(struct process *proc)
{
	putkpg(proc->kstack);
	putkpg(proc->pgdir);
}

/**
 * @brief Handles a validity page fault.
 * 
 * @brief addr Faulting address.
 * 
 * @returns Upon successful completion, zero is returned. Upon
 * failure, non-zero is returned instead.
 */
PUBLIC int vfault(addr_t addr)
{
	struct pte *pg;       /* Working page.           */
	struct region *reg;   /* Working region.         */
	struct pregion *preg; /* Working process region. */

	/* Get process region. */
	if ((preg = findreg(curr_proc, addr)) != NULL)
		lockreg(reg = preg->reg);
	else
	{
		addr_t addr2;

		addr2 = addr + PAGE_SIZE;

		/* Check for stack growth. */
		if ((preg = findreg(curr_proc, addr2)) == NULL)
			goto error0;

		lockreg(reg = preg->reg);

		/* Not a stack region. */
		if (preg != STACK(curr_proc))
			goto error1;

		/* Expand region. */
		if (growreg(curr_proc, preg, PAGE_SIZE))
			goto error1;
	}
	
	pg = getpte(curr_proc, addr);
	
	/* Should be demand fill or demand zero. */
	if (!(pte_is_fill(pg) || pte_is_zero(pg)))
		goto error1;
	
	/* Demand fill. */
	else if (pte_is_fill(pg))
	{
		if (readpg(reg, addr))
			goto error1;
	}

	/* Demand zero. */
	else
	{
		if (allocupg(addr, reg->mode & MAY_WRITE))
			goto error1;
	}

	unlockreg(reg);
	return (0);

error1:
	unlockreg(reg);
error0:
	return (-1);
}

/**
 * @brief Handles a protection page fault.
 * 
 * @brief addr Faulting address.
 * 
 * @returns Upon successful completion, zero is returned. Upon
 * failure, non-zero is returned instead.
 */
PUBLIC int pfault(addr_t addr)
{
	struct pte *pg;       /* Faulting page.          */
	struct pregion *preg; /* Working process region. */

	/* Outside virtual address space. */
	if ((preg = findreg(curr_proc, addr)) == NULL)
		goto error0;
	
	lockreg(preg->reg);

	pg = getpte(curr_proc, addr);

	/* Copy on write not enabled. */
	if (!cow_enabled(pg))
		goto error1;
		
	/* Copy page. */
	if (cow_disable(pg))
		goto error1;

	unlockreg(preg->reg);
	return(0);

error1:
	unlockreg(preg->reg);
error0:
	return (-1);
}
