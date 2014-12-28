/* PLT trampoline.  MIPS version.
   Copyright (C) 1996-2001, 2002, 2003, 2004, 2005
   Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Kazumoto Kojima <kkojima@info.kanagawa-u.ac.jp>.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library.  If not, see
   <http://www.gnu.org/licenses/>.  */

/*  FIXME: Profiling of shared libraries is not implemented yet.  */

#include <sysdep.h>
#include <link.h>
#include <elf.h>
#include <ldsodefs.h>
#include <dl-machine.h>
#include <sysdep-cancel.h>

/* Get link map for callers object containing STUB_PC.  */
static inline struct link_map *
elf_machine_runtime_link_map (ElfW(Addr) gpreg, ElfW(Addr) stub_pc)
{
  extern int _dl_mips_gnu_objects;

  /* got[1] is reserved to keep its link map address for the shared
     object generated by the gnu linker.  If all are such objects, we
     can find the link map from current GPREG simply.  If not so, get
     the link map for caller's object containing STUB_PC.  */

  if (_dl_mips_gnu_objects)
    {
      ElfW(Addr) *got = elf_mips_got_from_gpreg (gpreg);
      ElfW(Word) g1;

      g1 = ((ElfW(Word) *) got)[1];

      if ((g1 & ELF_MIPS_GNU_GOT1_MASK) != 0)
	{
	  struct link_map *l =
	    (struct link_map *) (g1 & ~ELF_MIPS_GNU_GOT1_MASK);
	  ElfW(Addr) base, limit;
	  const ElfW(Phdr) *p = l->l_phdr;
	  ElfW(Half) this, nent = l->l_phnum;

	  /* For the common case of a stub being called from the containing
	     object, STUB_PC will point to somewhere within the object that
	     is described by the link map fetched via got[1].  Otherwise we
	     have to scan all maps.  */
	  for (this = 0; this < nent; this++)
	    {
	      if (p[this].p_type == PT_LOAD)
		{
		  base = p[this].p_vaddr + l->l_addr;
		  limit = base + p[this].p_memsz;
		  if (stub_pc >= base && stub_pc < limit)
		    return l;
		}
	    }
	}
    }

    struct link_map *l;
    Lmid_t nsid;

    for (nsid = 0; nsid < DL_NNS; ++nsid)
      for (l = GL(dl_ns)[nsid]._ns_loaded; l != NULL; l = l->l_next)
	{
	  ElfW(Addr) base, limit;
	  const ElfW(Phdr) *p = l->l_phdr;
	  ElfW(Half) this, nent = l->l_phnum;

	  for (this = 0; this < nent; ++this)
	    {
	      if (p[this].p_type == PT_LOAD)
		{
		  base = p[this].p_vaddr + l->l_addr;
		  limit = base + p[this].p_memsz;
		  if (stub_pc >= base && stub_pc < limit)
		    return l;
		}
	    }
	}

  _dl_signal_error (0, NULL, NULL, "cannot find runtime link map");
  return NULL;
}

/* Define mips specific runtime resolver. The function __dl_runtime_resolve
   is called from assembler function _dl_runtime_resolve which converts
   special argument registers t7 ($15) and t8 ($24):
     t7  address to return to the caller of the function
     t8  index for this function symbol in .dynsym
   to usual c arguments.

   Other architectures call fixup from dl-runtime.c in
   _dl_runtime_resolve.  MIPS instead calls __dl_runtime_resolve.  We
   have to use our own version because of the way the got section is
   treated on MIPS (we've also got ELF_MACHINE_PLT defined).  */

/* The flag _dl_mips_gnu_objects is set if all dynamic objects are
   generated by the gnu linker. */
int _dl_mips_gnu_objects = 1;

/* This is called from assembly stubs below which the compiler can't see.  */
static ElfW(Addr)
__dl_runtime_resolve (ElfW(Word), ElfW(Word), ElfW(Addr), ElfW(Addr))
		  __attribute_used__;

static ElfW(Addr)
__dl_runtime_resolve (ElfW(Word) sym_index,
		      ElfW(Word) return_address,
		      ElfW(Addr) old_gpreg,
		      ElfW(Addr) stub_pc)
{
  struct link_map *l = elf_machine_runtime_link_map (old_gpreg, stub_pc);
  const ElfW(Sym) *const symtab
    = (const ElfW(Sym) *) D_PTR (l, l_info[DT_SYMTAB]);
  const char *strtab = (const void *) D_PTR (l, l_info[DT_STRTAB]);
  ElfW(Addr) *got
    = (ElfW(Addr) *) D_PTR (l, l_info[DT_PLTGOT]);
  const ElfW(Word) local_gotno
    = (const ElfW(Word)) l->l_info[DT_MIPS (LOCAL_GOTNO)]->d_un.d_val;
  const ElfW(Word) gotsym
    = (const ElfW(Word)) l->l_info[DT_MIPS (GOTSYM)]->d_un.d_val;
  const ElfW(Sym) *sym = &symtab[sym_index];
  struct link_map *sym_map;
  ElfW(Addr) value;

  /* FIXME: The symbol versioning stuff is not tested yet.  */
  if (__builtin_expect (ELFW(ST_VISIBILITY) (sym->st_other), 0) == 0)
    {
      switch (l->l_info[VERSYMIDX (DT_VERSYM)] != NULL)
	{
	default:
	  {
	    const ElfW(Half) *vernum =
	      (const void *) D_PTR (l, l_info[VERSYMIDX (DT_VERSYM)]);
	    ElfW(Half) ndx = vernum[sym_index] & 0x7fff;
	    const struct r_found_version *version = &l->l_versions[ndx];

	    if (version->hash != 0)
	      {
                /* We need to keep the scope around so do some locking.  This is
		   not necessary for objects which cannot be unloaded or when
		   we are not using any threads (yet).  */
		if (!RTLD_SINGLE_THREAD_P)
		  THREAD_GSCOPE_SET_FLAG ();

		sym_map = _dl_lookup_symbol_x (strtab + sym->st_name, l,
					       &sym, l->l_scope, version,
					       ELF_RTYPE_CLASS_PLT, 0, 0);

                /* We are done with the global scope.  */
		if (!RTLD_SINGLE_THREAD_P)
		  THREAD_GSCOPE_RESET_FLAG ();

		break;
	      }
	    /* Fall through.  */
	  }
	case 0:
	  {
          /* We need to keep the scope around so do some locking.  This is
	     not necessary for objects which cannot be unloaded or when
	     we are not using any threads (yet).  */
	  int flags = DL_LOOKUP_ADD_DEPENDENCY;
	  if (!RTLD_SINGLE_THREAD_P)
	    {
	      THREAD_GSCOPE_SET_FLAG ();
	      flags |= DL_LOOKUP_GSCOPE_LOCK;
	    }

	  sym_map = _dl_lookup_symbol_x (strtab + sym->st_name, l, &sym,
					 l->l_scope, 0, ELF_RTYPE_CLASS_PLT,
					 flags, 0);

          /* We are done with the global scope.  */
	  if (!RTLD_SINGLE_THREAD_P)
	    THREAD_GSCOPE_RESET_FLAG ();
	  }
	}

      /* Currently value contains the base load address of the object
	 that defines sym.  Now add in the symbol offset.  */
      value = (sym ? sym_map->l_addr + sym->st_value : 0);
    }
  else
    /* We already found the symbol.  The module (and therefore its load
       address) is also known.  */
    value = l->l_addr + sym->st_value;

  /* Apply the relocation with that value.  */
  *(got + local_gotno + sym_index - gotsym) = value;

  return value;
}

#if _MIPS_SIM == _ABIO32
#define ELF_DL_FRAME_SIZE 40

#define ELF_DL_SAVE_ARG_REGS "\
	sw	$15, 36($29)\n						      \
	sw	$4, 16($29)\n						      \
	sw	$5, 20($29)\n						      \
	sw	$6, 24($29)\n						      \
	sw	$7, 28($29)\n						      \
"

#define ELF_DL_RESTORE_ARG_REGS "\
	lw	$31, 36($29)\n						      \
	lw	$4, 16($29)\n						      \
	lw	$5, 20($29)\n						      \
	lw	$6, 24($29)\n						      \
	lw	$7, 28($29)\n						      \
"

/* The PLT resolver should also save and restore $2 and $3, which are used
   as arguments to MIPS16 stub functions.  */
#define ELF_DL_PLT_FRAME_SIZE 48

#define ELF_DL_PLT_SAVE_ARG_REGS \
	ELF_DL_SAVE_ARG_REGS "\
	sw	$2, 40($29)\n						      \
	sw	$3, 44($29)\n						      \
"

#define ELF_DL_PLT_RESTORE_ARG_REGS \
	ELF_DL_RESTORE_ARG_REGS "\
	lw	$2, 40($29)\n						      \
	lw	$3, 44($29)\n						      \
"

#define IFABIO32(X) X
#define IFNEWABI(X)

#else /* _MIPS_SIM == _ABIN32 || _MIPS_SIM == _ABI64 */

#define ELF_DL_FRAME_SIZE 80

#define ELF_DL_SAVE_ARG_REGS "\
	sd	$15, 72($29)\n						      \
	sd	$4, 8($29)\n						      \
	sd	$5, 16($29)\n						      \
	sd	$6, 24($29)\n						      \
	sd	$7, 32($29)\n						      \
	sd	$8, 40($29)\n						      \
	sd	$9, 48($29)\n						      \
	sd	$10, 56($29)\n						      \
	sd	$11, 64($29)\n						      \
"

#define ELF_DL_RESTORE_ARG_REGS "\
	ld	$31, 72($29)\n						      \
	ld	$4, 8($29)\n						      \
	ld	$5, 16($29)\n						      \
	ld	$6, 24($29)\n						      \
	ld	$7, 32($29)\n						      \
	ld	$8, 40($29)\n						      \
	ld	$9, 48($29)\n						      \
	ld	$10, 56($29)\n						      \
	ld	$11, 64($29)\n						      \
"

/* The PLT resolver should also save and restore $2 and $3, which are used
   as arguments to MIPS16 stub functions.  */
#define ELF_DL_PLT_FRAME_SIZE 96

#define ELF_DL_PLT_SAVE_ARG_REGS \
	ELF_DL_SAVE_ARG_REGS "\
	sd	$2, 80($29)\n						      \
	sd	$3, 88($29)\n						      \
"

#define ELF_DL_PLT_RESTORE_ARG_REGS \
	ELF_DL_RESTORE_ARG_REGS "\
	ld	$2, 80($29)\n						      \
	ld	$3, 88($29)\n						      \
"

#define IFABIO32(X)
#define IFNEWABI(X) X

#endif

asm ("\n\
	.text\n\
	.align	2\n\
	.globl	_dl_runtime_resolve\n\
	.type	_dl_runtime_resolve,@function\n\
	.ent	_dl_runtime_resolve\n\
_dl_runtime_resolve:\n\
	.frame	$29, " STRINGXP(ELF_DL_FRAME_SIZE) ", $31\n\
	.set noreorder\n\
	# Save GP.\n\
1:	move	$3, $28\n\
	# Save arguments and sp value in stack.\n\
	" STRINGXP(PTR_SUBIU) "  $29, " STRINGXP(ELF_DL_FRAME_SIZE) "\n\
	# Modify t9 ($25) so as to point .cpload instruction.\n\
	" IFABIO32(STRINGXP(PTR_ADDIU) "	$25, (2f-1b)\n") "\
	# Compute GP.\n\
2:	" STRINGXP(SETUP_GP) "\n\
	" STRINGXV(SETUP_GP64 (0, _dl_runtime_resolve)) "\n\
	.set reorder\n\
	# Save slot call pc.\n\
	move	$2, $31\n\
	" IFABIO32(STRINGXP(CPRESTORE(32))) "\n\
	" ELF_DL_SAVE_ARG_REGS "\
	move	$4, $24\n\
	move	$5, $15\n\
	move	$6, $3\n\
	move	$7, $2\n\
	jal	__dl_runtime_resolve\n\
	" ELF_DL_RESTORE_ARG_REGS "\
	" STRINGXP(RESTORE_GP64) "\n\
	" STRINGXP(PTR_ADDIU) "	$29, " STRINGXP(ELF_DL_FRAME_SIZE) "\n\
	move	$25, $2\n\
	jr	$25\n\
	.end	_dl_runtime_resolve\n\
	.previous\n\
");

/* Assembler veneer called from the PLT header code when using PLTs.

   Code in each PLT entry and the PLT header fills in the arguments to
   this function:

   - $15 (o32 t7, n32/n64 t3) - caller's return address
   - $24 (t8) - PLT entry index
   - $25 (t9) - address of _dl_runtime_pltresolve
   - o32 $28 (gp), n32/n64 $14 (t2) - address of .got.plt

   Different registers are used for .got.plt because the ABI was
   originally designed for o32, where gp was available (call
   clobbered).  On n32/n64 gp is call saved.

   _dl_fixup needs:

   - $4 (a0) - link map address
   - $5 (a1) - .rel.plt offset (== PLT entry index * 8)  */

asm ("\n\
	.text\n\
	.align	2\n\
	.globl	_dl_runtime_pltresolve\n\
	.type	_dl_runtime_pltresolve,@function\n\
	.ent	_dl_runtime_pltresolve\n\
_dl_runtime_pltresolve:\n\
	.frame	$29, " STRINGXP(ELF_DL_PLT_FRAME_SIZE) ", $31\n\
	.set noreorder\n\
	# Save arguments and sp value in stack.\n\
1:	" STRINGXP(PTR_SUBIU) "	$29, " STRINGXP(ELF_DL_PLT_FRAME_SIZE) "\n\
	" IFABIO32(STRINGXP(PTR_L) "	$13, " STRINGXP(PTRSIZE) "($28)") "\n\
	" IFNEWABI(STRINGXP(PTR_L) "	$13, " STRINGXP(PTRSIZE) "($14)") "\n\
	# Modify t9 ($25) so as to point .cpload instruction.\n\
	" IFABIO32(STRINGXP(PTR_ADDIU) "	$25, (2f-1b)\n") "\
	# Compute GP.\n\
2:	" STRINGXP(SETUP_GP) "\n\
	" STRINGXV(SETUP_GP64 (0, _dl_runtime_pltresolve)) "\n\
	.set reorder\n\
	" IFABIO32(STRINGXP(CPRESTORE(32))) "\n\
	" ELF_DL_PLT_SAVE_ARG_REGS "\
	move	$4, $13\n\
	sll	$5, $24, " STRINGXP(PTRLOG) " + 1\n\
	jal	_dl_fixup\n\
	move	$25, $2\n\
	" ELF_DL_PLT_RESTORE_ARG_REGS "\
	" STRINGXP(RESTORE_GP64) "\n\
	" STRINGXP(PTR_ADDIU) "	$29, " STRINGXP(ELF_DL_PLT_FRAME_SIZE) "\n\
	jr	$25\n\
	.end	_dl_runtime_pltresolve\n\
	.previous\n\
");

