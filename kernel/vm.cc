#include "types.h"
#include "amd64.h"
#include "mmu.h"
#include "cpu.hh"
#include "kernel.hh"
#include "bits.hh"
#include "spinlock.h"
#include "kalloc.hh"
#include "queue.h"
#include "condvar.h"
#include "proc.hh"
#include "vm.hh"
#include "gc.hh"
#include "crange.hh"
#include "cpputil.hh"
#include "sperf.hh"
#include "uwq.hh"
#include "kmtrace.hh"
#include "kstream.hh"

enum { vm_debug = 0 };
enum { tlb_shootdown = 1 };
enum { tlb_lazy = 1 };

/*
 * vmnode
 */

vmnode::vmnode(u64 npg, vmntype ntype, inode *i, u64 off, u64 s)
  : npages(npg), empty(true), type(ntype),
    offset(off), sz(s), ref_(0), ip(i)
{
  if (ip != nullptr)
    ip->inc();

  if (npg > NELEM(page))
    panic("vmnode too big\n");
  // XXX Maybe vmnode should take just a byte size
  if (ip && (sz > npages * PGSIZE || sz <= (npages - 1) * PGSIZE))
    panic("vmnode bad size %lu for npages %lu", sz, npages);
  memset(page, 0, npg * sizeof(page[0]));
  if (type == EAGER && ip) {
    allocall(false);
    assert(loadall() == 0);
  }
}

vmnode::~vmnode()
{
  for(u64 i = 0; i < npages; i++)
    if (page[i])
      kfree(page[i]);
  if (ip != nullptr)
    iput(ip);
}

void
vmnode::decref(void)
{
  if(--ref_ == 0)
    delete this;
}

void
vmnode::incref(void)
{
  ++ref_;
}

u64
vmnode::ref(void)
{
  return ref_.load();
}

int
vmnode::allocpg(int idx, bool zero)
{
  char* p;

  if (page[idx])
    return 0;
  
  if (zero)
    p = zalloc("(vmnode::allocall)");
  else
    p = kalloc("(vmnode::allocall)");

  if (p == nullptr)
    return -1;

  if(!cmpxch(&page[idx], (char*) 0, p)) {
    if (zero)
      zfree(p);
    else
      kfree(p);
  } else if (empty) {
    empty = false;
  }

  return 0;
}

void
vmnode::allocall(bool zero)
{
  for(u64 i = 0; i < npages; i++)
    if (allocpg(i, zero) < 0)
      throw_bad_alloc();
}

vmnode *
vmnode::copy()
{
  vmnode *c = new vmnode(npages, type,
                         (type==ONDEMAND) ? idup(ip) : 0,
                         offset, sz);
  if (empty)
    return c;

  auto cleanup = scoped_cleanup([&c]() { delete c; });
  c->allocall(false);

  for(u64 i = 0; i < npages; i++)
    if (page[i])
      memmove(c->page[i], page[i], PGSIZE);

  cleanup.dismiss();
  return c;
}

int
vmnode::loadpg(off_t off)
{
#ifdef MTRACE 
  mtreadavar("inode:%x.%x", ip->dev, ip->inum);
  mtwriteavar("vmnode:%p", this);
#endif

  assert(off <= sz);

  char *p = page[off/PGSIZE];
  s64 filen = MIN(PGSIZE, sz-off);
  off_t fileo = offset+off;

  //
  // Possible race condition with concurrent loadpg() calls,
  // if the underlying inode's contents change..
  //
  if (readi(ip, p, fileo, filen) != filen)
    return -1;

  // XXX(sbw), we should memset the remainder, but sometimes
  // we loadpg on a pg that already has content.
  //memset(&p[filen], 0, PGSIZE-filen);
  return 0;
}


int
vmnode::loadall()
{
  for (off_t o = 0; o < sz; o += PGSIZE)
    if (loadpg(o) < 0)
      return -1;
  return 0;
}

/*
 * vma
 */

vma::vma(vmap *vmap, uptr start, uptr end, enum vmatype vtype, vmnode *vmn) :
#if VM_CRANGE
    range(&vmap->vmas, start, end-start),
#endif
    vma_start(start), vma_end(end), va_type(vtype), n(vmn)
{
  assert(PGOFFSET(start) == 0);
  assert(PGOFFSET(end) == 0);
  assert(!vmn || end - start == vmn->npages << PGSHIFT);
  if (n)
    n->incref();
}

vma::~vma()
{
  if (n)
    n->decref();
}

void
to_stream(print_stream *s, vma *v)
{
  s->print("vma@[", shex(v->vma_start), ',', shex(v->vma_end), ')',
           v->va_type == COW ? "/COW" : "");
}

/*
 * vmap
 */

vmap*
vmap::alloc(void)
{
  return new vmap();
}

vmap::vmap() : 
#if VM_CRANGE
  vmas(10),
#endif
#if VM_RADIX
  vmas(PGSHIFT),
#endif
  ref(1), pml4(setupkvm()), kshared((char*) ksalloc(slab_kshared)),
  brk_(0)
{
  initlock(&brklock_, "brk_lock", LOCKSTAT_VM);
  if (pml4 == 0) {
    cprintf("vmap_alloc: setupkvm out of memory\n");
    goto err;
  }

  if (kshared == nullptr) {
    cprintf("vmap::vmap: kshared out of memory\n");
    goto err;
  }

  if (mapkva(pml4, kshared, KSHARED, KSHAREDSIZE)) {
    cprintf("vmap::vmap: mapkva out of memory\n");
    goto err;
  }

  return;

 err:
  if (kshared)
    ksfree(slab_kshared, kshared);
  if (pml4)
    freevm(pml4);
  throw_bad_alloc();
}

vmap::~vmap()
{
  if (kshared)
    ksfree(slab_kshared, kshared);
  if (pml4)
    freevm(pml4);
  destroylock(&brklock_);
}

void
vmap::decref()
{
  if (--ref == 0)
    delete this;
}

void
vmap::incref()
{
  ++ref;
}

bool
vmap::replace_vma(vma *a, vma *b)
{
  assert(a->vma_start == b->vma_start);
  assert(a->vma_end == b->vma_end);
  auto span = vmas.search_lock(a->vma_start, a->vma_end - a->vma_start);
  if (a->deleted())
    return false;
#if VM_CRANGE
  for (auto e: span)
    assert(a == e);
  span.replace(b);
#endif
#if VM_RADIX
  for (auto it = span.begin(); it != span.end(); ++it) {
    if (static_cast<vma*>(*it) == a)
      // XXX(austin) replace should take iterators to represent the
      // span so we don't have to find the keys all over again.
      span.replace(it.key(), it.span(), b);
  }
#endif
  return true;
}

vmap*
vmap::copy(int share)
{
  vmap *nm = new vmap();

#if VM_RADIX
  radix::iterator next_it;
  for (auto it = vmas.begin(); it != vmas.end(); it = next_it, it.skip_nulls()) {
    next_it = it.next_change();
    u64 range_start = it.key();
    u64 range_end = next_it.key();
    vma *e = static_cast<vma*>(*it);
#endif
#if 0
  }  // Ugh.  Un-confuse IDE indentation.
#endif
#if VM_CRANGE
  for (auto r: vmas) {
    vma *e = static_cast<vma *>(r);
    u64 range_start = e->vma_start;
    u64 range_end = e->vma_end;
#endif
    u64 range_size = range_end - range_start;

    struct vma *ne;
    if (share) {
      // Because of the pages array, the new vma needs to have the
      // same start and end, even if that's not where it ends up in
      // the index.
      ne = new vma(nm, e->vma_start, e->vma_end, COW, e->n);

      // if the original vma wasn't COW, replace it with a COW vma
      if (e->va_type != COW) {
        vma *repl = new vma(this, e->vma_start, e->vma_end, COW, e->n);
#if VM_RADIX
        vmas.search_lock(range_start, range_size).replace(range_start, range_size, repl);
#elif VM_CRANGE
        replace_vma(e, repl);
#endif
        updatepages(pml4, range_start, range_end, [](atomic<pme_t>* p) {
            for (;;) {
              pme_t v = p->load();
              if (v & PTE_LOCK)
                continue;
              if (!(v & PTE_P) || !(v & PTE_U) || !(v & PTE_W) ||
                  cmpxch(p, v, PTE_ADDR(v) | PTE_P | PTE_U | PTE_COW))
                break;
            }
          });
      }
    } else {
      // XXX free vmnode copy if vma alloc fails
      ne = new vma(nm, e->vma_start, e->vma_end, PRIVATE, e->n->copy());
    }

    auto span = nm->vmas.search_lock(range_start, range_size);
    for (auto x: span) {
#if VM_RADIX
      if (!x)
        continue;
#endif
      cprintf("non-empty span 0x%lx--0x%lx in %p: %p\n",
              ne->vma_start, ne->vma_end, nm, x);
      assert(0);  /* span must be empty */
    }
#if VM_CRANGE
    span.replace(ne);
#endif
#if VM_RADIX
    span.replace(range_start, range_size, ne);
#endif
  }

  if (share) {
    // Reload hardware page table
    if (tlb_shootdown) {
      tlbflush();
    } else {
      lcr3(rcr3());
    }
  }

  nm->brk_ = brk_;
  return nm;
}

// Does any vma overlap start..start+len?
// If yes, return the vma pointer.
// If no, return 0.
// This code can't handle regions at the very end
// of the address space, e.g. 0xffffffff..0x0
// We key vma's by their end address.
vma *
vmap::lookup(uptr start, uptr len)
{
  if (start + len < start)
    panic("vmap::lookup bad len");

#if VM_CRANGE
  auto r = vmas.search(start, len);
#endif
#if VM_RADIX
  assert(len <= PGSIZE);
  auto r = vmas.search(start);
#endif
  if (r != 0) {
    vma *e = (vma *) r;
    if (e->vma_end <= e->vma_start)
      panic("malformed va");
    if (e->vma_start < start+len && e->vma_end > start)
      return e;
  }

  return 0;
}

long
vmap::insert(vmnode *n, uptr vma_start, int dotlb)
{
  ANON_REGION("vmap::insert", &perfgroup);

  vma *e;
  bool replaced = false;
  bool fixed = (vma_start != 0);

again:
  if (!fixed) {
    vma_start = unmapped_area(n->npages);
    if (vma_start == 0) {
      cprintf("vmap::insert: no unmapped areas\n");
      return -1;
    }
  }

  {
    // new scope to release the search lock before tlbflush
    u64 len = n->npages * PGSIZE;
    auto span = vmas.search_lock(vma_start, len);
#if VM_CRANGE
    // XXX handle overlaps, set replaced=true
    for (auto r: span) {
      if (!fixed)
        goto again;

      vma *rvma = (vma*) r;
      uerr.println("vmap::insert: overlap with ", rvma);
      return -1;
    }
#endif
#if VM_RADIX
    // XXX(austin) span.replace also has to do this scan.  It would be
    // nice if we could do just one scan.
    for (auto r: span) {
      if (!r)
        continue;
      if (!fixed)
        goto again;
      else {
        // XXX(austin) I don't think anything prevents a page fault
        // from reading the old VMA now and installing the new page
        // for the old VMA after the updatepages.  Certainly not
        // PTE_LOCK, since we don't take that here.  Why not just use
        // the lock in the radix tree?  (We can't do that with crange,
        // though, since it can only lock complete ranges.)
        replaced = true;
        break;
      }
    }
#endif

    e = new vma(this, vma_start, vma_start+len, PRIVATE, n);
    if (e == 0) {
      cprintf("vmap::insert: out of vmas\n");
      return -1;
    }

#if VM_CRANGE
    span.replace(e);
#endif
#if VM_RADIX
    span.replace(e->vma_start, e->vma_end-e->vma_start, e);
#endif
  }

  bool needtlb = false;
  if (replaced)
    updatepages(pml4, e->vma_start, e->vma_end, [&needtlb](atomic<pme_t> *p) {
        for (;;) {
          pme_t v = p->load();
          // XXX(austin) Huh?  Why is it okay to skip it if it's
          // locked?  The page fault could be faulting in a page from
          // the old VMA, in which case we need to shoot it down
          // (though if it's already faulting a page from the new VMA,
          // we need to *not* shoot it down).
          if (v & PTE_LOCK)
            continue;
          if (!(v & PTE_P))
            break;
          if (cmpxch(p, v, (pme_t) 0)) {
            needtlb = true;
            break;
          }
        }
      });
  if (tlb_shootdown) {
    if (needtlb && dotlb)
      tlbflush();
    else
      if (tlb_lazy)
        tlbflush(myproc()->unmap_tlbreq_);
  }

  return vma_start;
}

int
vmap::remove(uptr vma_start, uptr len)
{
  {
    // new scope to release the search lock before tlbflush

    auto span = vmas.search_lock(vma_start, len);
#if VM_CRANGE
    // XXX handle partial unmap
    uptr vma_end = vma_start + len;
    for (auto r: span) {
      vma *rvma = (vma*) r;
      if (rvma->vma_start < vma_start || rvma->vma_end > vma_end) {
        uerr.println("vmap::remove: partial unmap not supported; "
                     "unmapping [", shex(vma_start),",",shex(vma_start+len), ")"
                     " from ", rvma);
        return -1;
      }
    }
#endif

#if VM_CRANGE
    span.replace(0);
#endif
#if VM_RADIX
    // XXX(austin) If this could tell us that nothing was replaced, we
    // could skip the updatepages.
    span.replace(vma_start, len, 0);
#endif
  }

  bool needtlb = false;
  updatepages(pml4, vma_start, vma_start + len, [&needtlb](atomic<pme_t> *p) {
      for (;;) {
        pme_t v = p->load();
        if (v & PTE_LOCK)
          continue;
        if (!(v & PTE_P))
          break;
        if (cmpxch(p, v, (pme_t) 0)) {
          needtlb = true;
          break;
        }
      }
    });
  if (tlb_shootdown && needtlb) {
    if (tlb_lazy) {
      myproc()->unmap_tlbreq_ = tlbflush_req++;
    } else {
      tlbflush();
    }
  }
  return 0;
}

/*
 * pagefault handling code on vmap
 */

int
vmap::pagefault_wcow(vma *m)
{
  // Always make a copy of n, even if this process has the only ref, 
  // because other processes may change ref count while this process 
  // is handling wcow.
  struct vmnode *nodecopy = m->n->copy();

  vma *repl = new vma(this, m->vma_start, m->vma_end, PRIVATE, nodecopy);

  // XXX(austin) This will cause sharing on parts of this range that
  // have since been unmapped or replaced.  But in our current design
  // where we need a new vmnode we have to replace all instances of it
  // at once or we'll end up with a complete vmnode copy for each page
  // we fault on.  If we replace it all at once, this will waste time
  // and space copying pages that are no longer mapped, but will only
  // do that once.  Fixing this requires getting rid of the vmnode.
  replace_vma(m, repl);
  updatepages(pml4, m->vma_start, m->vma_end, [](atomic<pme_t> *p) {
      // XXX(austin) In radix, this may clear PTEs belonging to other
      // VMAs that have replaced sub-ranges of the faulting VMA.
      // That's unfortunate but okay because we'll just bring them
      // back from the pages array.  Yet another consequence of having
      // to do a vmnode at a time.
      for (;;) {
        pme_t v = p->load();
        if (v & PTE_LOCK)
          continue;
        if (cmpxch(p, v, (pme_t) 0))
          break;
      }
    });

  return 0;
}

int
vmap::pagefault(uptr va, u32 err)
{
  if (va >= USERTOP)
    return -1;

  atomic<pme_t> *pte = walkpgdir(pml4, va, 1);
  if (pte == nullptr)
    throw_bad_alloc();

 retry:
  pme_t ptev = pte->load();

  // optimize checks of args to syscals
  if ((ptev & (PTE_P|PTE_U|PTE_W)) == (PTE_P|PTE_U|PTE_W)) {
    // XXX using pagefault() as a security check in syscalls is prone to races
    return 0;
  }

  if (ptev & PTE_LOCK) {
    // locked, might as well wait for the other pagefaulting core..
    goto retry;
  }

  scoped_gc_epoch gc;
  vma *m = lookup(va, 1);
  if (m == 0)
    return -1;

  u64 npg = (PGROUNDDOWN(va) - m->vma_start) / PGSIZE;
  if (vm_debug)
    cprintf("pagefault: err 0x%x va 0x%lx type %d ref %lu pid %d\n",
            err, va, m->va_type, m->n->ref(), myproc()->pid);

  if (m->va_type == COW && (err & FEC_WR)) {
    if (pagefault_wcow(m) < 0)
      return -1;

    if (tlb_shootdown) {
      tlbflush();
    } else {
      lcr3(rcr3());
    }
    goto retry;
  }

  if (m->n && !m->n->page[npg])
    // XXX(sbw) you might think we don't need to zero if ONDEMAND;
    // however, our vmnode might include not backed by a file
    // (e.g. the bss section of an ELF segment)
    // XXX(austin) Why is this still necessary now that we map zeroed
    // parts of segments separately?
    if (m->n->allocpg(npg, true) < 0)
      throw_bad_alloc();

  // XXX(sbw) If m->n->page[npg] has contents (e.g. was loaded in
  // a parent before fork) we shouldn't call loadpg
  if (m->n && m->n->type == ONDEMAND)
    if (m->n->loadpg(npg*PGSIZE) < 0) {
      cprintf("pagefault: couldn't load\n");
      return -1;
    }

  if (!cmpxch(pte, ptev, ptev | PTE_LOCK))
    goto retry;

  if (m->deleted()) {
    *pte = ptev;
    goto retry;
  }

  if (m->va_type == COW) {
    *pte = v2p(m->n->page[npg]) | PTE_P | PTE_U | PTE_COW;
  } else {
    *pte = v2p(m->n->page[npg]) | PTE_P | PTE_U | PTE_W;
  }

  mtreadavar("vmnode:%p", m->n);

  return 1;
}

int
pagefault(vmap *vmap, uptr va, u32 err)
{
#if MTRACE
  mt_ascope ascope("%s(%#lx)", __func__, va);
  mtwriteavar("pte:%p.%#lx", vmap, va / PGSIZE);
#endif

  for (;;) {
#if EXCEPTIONS
    try {
#endif
      return vmap->pagefault(va, err);
#if EXCEPTIONS
    } catch (retryable& e) {
      cprintf("%d: pagefault retry\n", myproc()->pid);
      gc_wakeup();
      yield();
    }
#endif
  }
}

void*
vmap::pagelookup(uptr va)
{
  if (va >= USERTOP)
    return nullptr;

  scoped_gc_epoch gc;
  vma *m = lookup(va, 1);
  if (m == 0 || m->n == nullptr)
    return nullptr;

  u64 npg = (PGROUNDDOWN(va)-m->vma_start) / PGSIZE;

  if (!m->n->page[npg])
    // XXX(sbw) you might think we don't need to zero if ONDEMAND;
    // however, our vmnode might include not backed by a file
    // (e.g. the bss section of an ELF segment)
    // XXX(austin) Why is this still necessary now that we map zeroed
    // parts of segments separately?
    if (m->n->allocpg(npg, true) < 0)
      throw_bad_alloc();

  char* kptr = (char*)(m->n->page[npg]);
  mtreadavar("vmnode:%p", m->n);
  return &kptr[va & (PGSIZE-1)];
}

void*
pagelookup(vmap* vmap, uptr va)
{
#if MTRACE
  mt_ascope ascope("%s(%#lx)", __func__, va);
  mtwriteavar("pte:%p.%#lx", vmap, va / PGSIZE);
#endif

  for (;;) {
#if EXCEPTIONS
    try {
#endif
      return vmap->pagelookup(va);
#if EXCEPTIONS
    } catch (retryable& e) {
      cprintf("%d: pagelookup retry\n", myproc()->pid);
      gc_wakeup();
      yield();
    }
#endif
  }
}

// Copy len bytes from p to user address va in vmap.
// Most useful when vmap is not the current page table.
int
vmap::copyout(uptr va, void *p, u64 len)
{
  char *buf = (char*)p;
  while(len > 0){
    uptr va0 = (uptr)PGROUNDDOWN(va);
    scoped_gc_epoch gc;
    vma *vma = lookup(va, 1);
    if(vma == 0)
      return -1;

    vma->n->allocall();
    uptr pn = (va0 - vma->vma_start) / PGSIZE;
    char *p0 = vma->n->page[pn];
    if(p0 == 0)
      panic("copyout: missing page");
    uptr n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(p0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

int
vmap::sbrk(ssize_t n, uptr *addr)
{
  scoped_acquire xlock(&brklock_);
  auto curbrk = brk_;
  *addr = curbrk;

  if(n <= 0 && 0 - n <= curbrk){
    brk_ += n;
    return 0;
  }

  if(n <= 0 || n > USERTOP || curbrk + n > USERTOP)
    return -1;

  // look one page ahead, to check if the newly allocated region would
  // abut the next-higher vma? we can't allow that, since then a future
  // sbrk() would start to use the next region (e.g. the stack).
  uptr newstart = PGROUNDUP(curbrk);
  s64 newn = PGROUNDUP(n + curbrk - newstart);
#if VM_CRANGE
  range *prev = 0;
#endif
#if VM_RADIX
  void *last = 0;
#endif
  auto span = vmas.search_lock(newstart, newn + PGSIZE);
  for (auto r: span) {
#if VM_RADIX
    if (!r || r == last)
      continue;
    last = r;
#endif
    vma *e = (vma*) r;

    if (e->vma_start <= newstart) {
      if (e->vma_end >= newstart + newn) {
        brk_ += n;
        return 0;
      }

      newn -= e->vma_end - newstart;
      newstart = e->vma_end;
#if VM_CRANGE
      prev = e;
#endif
    } else {
      uerr.println("growproc: overlap with existing mapping; "
                   "brk ", shex(curbrk), " n ", n);
      return -1;
    }
  }

  vmnode *vmn = new vmnode(newn / PGSIZE);
  if(vmn == 0){
    cprintf("growproc: vmn_allocpg failed\n");
    return -1;
  }

  vma *repl = new vma(this, newstart, newstart+newn, PRIVATE, vmn);
  if (!repl) {
    cprintf("growproc: out of vma\n");
    delete vmn;
    return -1;
  }

#if VM_CRANGE
  span.replace(prev, repl);
#endif

#if VM_RADIX
  span.replace(newstart, newn, repl);
#endif

  brk_ += n;
  return 0;
}

uptr
vmap::unmapped_area(size_t npages)
{
  size_t n = npages*PGSIZE;
  uptr addr = 0x1000;

  while (addr < USERTOP) {
#if VM_CRANGE
    auto x = vmas.search(addr, n);
    if (x == nullptr)
      return addr;
    vma* a = (vma*) x;
    addr = a->vma_end;
#endif

#if VM_RADIX
    bool overlap = false;
    for (uptr ax = addr; ax < addr+n; ax += PGSIZE) {
      auto x = vmas.search(ax);
      if (x != nullptr) {
        overlap = true;
        vma* a = (vma*) x;
        addr = a->vma_end;
        break;
      }
    }
    if (!overlap)
      return addr;
#endif
  }
  return 0;
}
