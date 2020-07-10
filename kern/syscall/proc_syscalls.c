#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include "opt-A2.h"
#include <mips/trapframe.h>
#include <synch.h>
#include <vfs.h>
#include <kern/fcntl.h>

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);


  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

#if OPT_A2
  // update current process structure
  lock_acquire(p->child_lock);
    p->exitCode = exitcode;
    p->dead = true;

  if(p->parent) {
    // if the parent is alive, then wake them up
      cv_broadcast(p->p_cv, p->child_lock);
  }
  lock_release(p->child_lock);

  lock_acquire(destroyLock);
  if (p->parent == NULL) {
    proc_destroy(p);
  }
  lock_release(destroyLock);
#else
  (void)exitcode;
#endif

  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}

/* handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
#if OPT_A2
  *retval = curproc->PID;
#else
  *retval = 1;
#endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }

#if OPT_A2
    lock_acquire(curproc->child_lock);
	struct proc * child;
	for (unsigned int i = 0; i < array_num(curproc->children); ++i) {
	    struct proc * temp = array_get(curproc->children, i);

	    // pid is found and a child of the current process
	    if (temp->PID == pid) {
		child = temp;
	        break;
	    }
	}
    lock_release(curproc->child_lock);

     // the pid is not a child of the current process
     if (child == NULL) {
	*retval = -1;
	return ECHILD;
     }

    // if child is currently alive, fall asleep until child dies
    lock_acquire(child->child_lock);
	while (!child->dead) {
	    cv_wait(child->p_cv, child->child_lock);
	}

        exitstatus = _MKWAIT_EXIT(child->exitCode);
  lock_release(child->child_lock);
#else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
#endif
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

#if OPT_A2
int
sys_fork(struct trapframe *tf,
		pid_t *retval)
{
	// create a new process for the child process
	struct proc * new_proc = proc_create_runprogram(curproc->p_name);
	if (new_proc == NULL) {
		return ENOMEM;
	}

	// copy the parent's address space to the child
	as_copy(curproc_getas(), &new_proc->p_addrspace);
	if (new_proc->p_addrspace == NULL) {
		proc_destroy(new_proc);
		return ENOMEM;
	}

	//establish parent-child relationship
	lock_acquire(curproc->child_lock);
		array_add(curproc->children, new_proc, NULL);
	lock_release(curproc->child_lock);

	new_proc->parent = curproc;

	// create a temp copy of the trapfram onto the OS heap
	new_proc->tf = kmalloc(sizeof(struct trapframe));
	memcpy(new_proc->tf, tf, sizeof(struct trapframe));

	// create threads
	thread_fork(new_proc->p_name,
			new_proc,
			&enter_forked_process_wrapper,
			(void *) new_proc->tf,
			1
		   );
	*retval = new_proc->PID;

	return 0;
}

int 
sys_execv(const char * progname, char ** args) 
{
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Copy the program path from user to kernel */
	char * progname_kern = kmalloc((strlen(progname)+1) * sizeof(char));
	copyin((const_userptr_t) progname, (void *) progname_kern, (strlen(progname)+1) * sizeof(char));

	/* Count the number of args and copy them into the kernel */
	int len = 0;
	while (args[len]) {
		++len;
	}
	char ** args_kern = kmalloc((len+1) * sizeof(char *));
	for (int i = 0; i < len; ++i) {
		args_kern[i] = kmalloc((strlen(args[i])+1) * sizeof(char));
		result = copyin((const_userptr_t) args[i], (void *) args_kern[i], (strlen(args[i])+1) * sizeof(char));
		if (result) return result;
	}
	args_kern[len] = NULL;

	/* Open the file. */
	result = vfs_open(progname_kern, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	struct addrspace * old_as = curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	/* Copy args from kernel to stack */
	vaddr_t *args_ptr = kmalloc((len+1) * sizeof(vaddr_t));
	for (int i = len-1; i >= 0; --i) {
		stackptr = stackptr - ROUNDUP(strlen(args_kern[i])+1, 4);
		result = copyoutstr(args_kern[i],
			   (userptr_t)stackptr,
			   strlen(args_kern[i]) + 1,
			   NULL);
		if (result) return result;
		args_ptr[i] = stackptr;
	}
	args_ptr[len] = (vaddr_t)NULL;

	for (int i = len; i >= 0; --i) {
		stackptr = stackptr - ROUNDUP(sizeof(vaddr_t), 4);
		result = copyout((void *) &args_ptr[i],
				(userptr_t) stackptr,
				sizeof(vaddr_t));
		if (result) return result;
	}

	/* Delete the old address space */
	as_destroy(old_as);
	
	/* Warp to user mode. */
	enter_new_process(len /*argc*/, (userptr_t)stackptr /*userspace addr of argv*/,
			  ROUNDUP(stackptr,8), entrypoint);
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;

}

#endif
