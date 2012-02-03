#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#define SYSWRITE_BSIZE 256

static void syscall_handler (struct intr_frame *);

static inline void* frame_arg (struct intr_frame *f, int i) 
{
  return ((uint32_t*)f->esp) + i;
}

static uint32_t get_frame_syscall (struct intr_frame *f) 
{
  return *(uint32_t*)frame_arg (f, 0);
}

/* Reads a byte at user virtual address UADDR. UADDR must be below
   PHYS_BASE.  Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}
 
/* Writes BYTE to user address UDST. UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

/* Reads a byte at user virtual address UADDR. Returns the byte value
   if successful, -1 if address was invalid. */
static int
get_byte (const uint8_t *uaddr)
{
  if (((void*)uaddr) < PHYS_BASE)
    return get_user (uaddr);
  else
    return -1;
}

/* Writes BYTE to user address UDST. Returns true if successful, false
   if unsuccessful. */
static bool
put_byte (uint8_t *udst, uint8_t byte)
{
  if ((void*)udst < PHYS_BASE)
    return put_user (udst, byte);
  else
    return false;
}

/* Like memcpy, but copies from userland */
static size_t
user_memcpy (void *dst, const void *src, size_t size)
{
  size_t i;
  int result;
  char byte;

  ASSERT (dst != NULL);
  ASSERT (src != NULL);

  byte = 0;
  for (i = 0; i < size; i++)
  {
    /* Make sure memory access was valid */
    result = get_byte ((uint8_t*)src + i);
    if (result == -1) break;

    /* Read the byte */
    byte = (char)result;
    ((char*)dst)[i] = byte;
  }

  return i;
}

/**
 * Terminates Pintos by calling shutdown_power_off().
 *
 * Arguments:
 * - none
 * Returns: 
 * - none
 */
static void
sys_halt (struct intr_frame *f UNUSED)
{
  shutdown_power_off ();
}

/**
 * Terminates the current user program, returning status to the kernel.
 *
 * Arguments: 
 * - int status: status that is returned to the parent process
 * Returns: 
 * - none
 */
static void
sys_exit (struct intr_frame *f)
{
  int status = *((int*)frame_arg (f, 1));
  struct process_status *ps = thread_current ()->p_status;
  if (ps != NULL)
  {
    /* Update status and notify any waiting parent of this */
    lock_acquire (&ps->l);
    ps->status = status;
    ps->t = NULL;
    cond_signal (&ps->cond, &ps->l);
    lock_release (&ps->l);
  }
  process_exit ();
}

/**
 * Runs the executable whose name is given in cmd_line, passing any given
 * arguments, and returns the new process's program id (pid).
 *
 * Arguments: 
 * - const char *cmd_line: the command line to invoke.
 * Returns: 
 * - the new process' pid, or -1 if the program cannot load or run.
 */
static int
sys_exec (struct intr_frame *f)
{
  /* Copy commandline from user to kernel space */
  char *user_cmdline = *((char**)frame_arg (f, 1));
  char *kern_cmdline = palloc_get_page (0);
  if (kern_cmdline == NULL) return -1;
  user_memcpy (kern_cmdline, user_cmdline, PGSIZE);

  /* Execute the process */
  tid_t tid = process_execute (kern_cmdline);

  /* Clean up and return results */
  palloc_free_page (kern_cmdline); 
  return (tid == TID_ERROR) ? -1 : tid;
}

/**
 * Waits for a child process pid and retrieves the child's exit status.
 *
 * Arguments: 
 * - int pid: the pid of the child process to wait on
 * Returns: 
 * - the exit status of the child process, or -1 if it not a valid child process
 */
static int
sys_wait (struct intr_frame *f UNUSED)
{
  int tid = *((int*)frame_arg (f, 1));
  return process_wait (tid);
}


static struct process_status*
get_cur_process (struct intr_frame *f UNUSED) 
{
  // TODO: Verify that it is actually possible to grab the current
  // thread like this here
  struct thread *cur_thread = thread_current ();
  return cur_thread->p_status;
}

static bool
sys_create (struct intr_frame *f)
{
  const char *filename = *(char**)frame_arg (f, 1);
  uint32_t initial_size = *(uint32_t*)frame_arg (f, 2);

  return filesys_create (filename, initial_size);
}

static bool 
sys_remove (struct intr_frame *f) 
{
  const char *filename = *(char**)frame_arg (f, 1);
  return filesys_remove (filename);
}

static int32_t 
sys_open (struct intr_frame *f) 
{
  const char *filename = *(char**)frame_arg (f, 1);
  struct file* file = filesys_open (filename); 

  if (file == NULL) return -1;

  int fd = process_add_file (get_cur_process (f), file);
  return fd;
}

static int32_t
sys_filesize (struct intr_frame *f) 
{
  int fd = *(int*)frame_arg (f, 1);

  struct file* file = process_get_file (get_cur_process (f), fd);
  if (file == NULL) return -1;

  return file_length (file);
}


static void
sys_seek (struct intr_frame *f) 
{
  int fd = *(int*)frame_arg (f, 1);
  off_t pos = *(off_t*)frame_arg (f, 2);

  struct file* file = process_get_file (get_cur_process (f), fd);
  if (file == NULL) return;
 
  file_seek (file, pos);
}


static uint32_t
sys_tell (struct intr_frame *f) 
{
  int fd = *(int*)frame_arg (f, 1);

  struct file* file = process_get_file (get_cur_process (f), fd);
  if (file == NULL) return -1;
 
  return file_tell (file);
}

static void
sys_close (struct intr_frame *f) 
{
  int fd = *(int*)frame_arg (f, 1);

  struct file* file = process_get_file (get_cur_process (f), fd);
  if (file == NULL) return;

  file_close (file);
  process_remove_file (get_cur_process (f), fd);
}


static int32_t 
sys_read (struct intr_frame *f UNUSED) 
{
  // TODO: Actually implement sys_read
  return -1;
}


/* This defines the prototype for functions that write out to some
   destination. The first argument is the address of the data,
   the second is the size of the data, and the third is auxiliary.
   The function should return the number of bytes it was able 
   to write. */
typedef int (*write_blocks_fn) (char*, size_t, void*);

static int
write_blocks_fd1 (char* kernel_buffer, size_t size, void *aux UNUSED) 
{
  putbuf (kernel_buffer, size);
  return size;
}

static int
write_blocks_fdgen (char* kernel_buffer, size_t size, void* aux) 
{
  struct file *file = (struct file*) aux;
  return 0;
}

static int32_t
sys_write_blocks (char* user_buffer, size_t size_total, 
                    write_blocks_fn writer, void *aux) 
{
  size_t size_remain = size_total;
  
  uint32_t result = 0;
  char kernel_buffer[SYSWRITE_BSIZE];
  while (size_remain > 0)
  {
    size_t bytes_attempt = 
      SYSWRITE_BSIZE > size_remain ? size_remain : SYSWRITE_BSIZE;

    size_t bytes_copied = 
      user_memcpy (kernel_buffer, user_buffer, bytes_attempt);

    size_t bytes_written = writer (kernel_buffer, bytes_copied, aux);

    result += bytes_written;
    if (bytes_written < bytes_attempt) break;

    size_remain -= bytes_copied;
  }

  return result;
}

static int32_t
sys_write (struct intr_frame *f) 
{
  int fd = *(int*)frame_arg (f, 1);
  char* user_buffer = *(char**) frame_arg (f, 2);
  size_t size_total = *(size_t*) frame_arg (f, 3);

  if (fd == 1) 
  {
    return sys_write_blocks (user_buffer, size_total, 
        write_blocks_fd1, NULL);
  } 

  struct file* file = process_get_file (get_cur_process (f), fd);
  if (file == NULL) return 0;

  return sys_write_blocks (user_buffer, size_total, 
      write_blocks_fdgen, file);
}


/* Registers the system call handler for internal interrupts. */
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Convenience bit-mashing method */
static uint32_t int_to_uint32_t (int i)
{
  return *((uint32_t*)i);
}

/* Handles system calls using the internal interrupt mechanism. The
   supported system calls are defined in lib/user/syscall.h */
static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t syscall = get_frame_syscall (f);
  uint32_t eax = f->eax;

  switch (syscall) 
  {
    case SYS_HALT:
      sys_halt (f);
      break;
    case SYS_EXIT:
      sys_exit (f);
      break;
    case SYS_EXEC:
      eax = int_to_uint32_t (sys_exec (f));
      break;
    case SYS_WAIT:
      eax = int_to_uint32_t (sys_wait (f));
      break;
    case SYS_CREATE:
      eax = sys_create (f);
      break;
    case SYS_REMOVE:
      eax = sys_remove (f);
      break;
    case SYS_OPEN:
      eax = sys_open (f);
      break;
    case SYS_FILESIZE:
      eax = sys_filesize (f);
      break;
    case SYS_READ:
      eax = sys_read (f);
      break;
    case SYS_WRITE:
      eax = sys_write (f);
      break;
    case SYS_SEEK:
      sys_seek (f);
      break;
    case SYS_TELL:
      sys_tell (f);
      break;
    case SYS_CLOSE:
      sys_close (f);
      break;
  }

  f->eax = eax;
}
