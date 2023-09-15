/*
  Based on https://github.com/travisdowns/robsize
  Henry Wong <henry@stuffedcow.net>
  http://blog.stuffedcow.net/2013/05/measuring-rob-capacity/
  2014-10-14
*/

#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libkern/OSCacheControl.h>
struct proc_threadcounts_data {
    uint64_t ptcd_instructions;
    uint64_t ptcd_cycles;
    uint64_t ptcd_user_time_mach;
    uint64_t ptcd_system_time_mach;
    uint64_t ptcd_energy_nj;
};

struct proc_threadcounts {
    uint16_t ptc_len;
    uint16_t ptc_reserved0;
    uint32_t ptc_reserved1;
    struct proc_threadcounts_data ptc_counts[];
};

#define PROC_PIDTHREADCOUNTS 34
#define PROC_PIDTHREADCOUNTS_SIZE (sizeof(struct proc_threadcounts))
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

static int its = 8192 * 4 * 2 ;
static int outer_its = 64 * 2 ;
static int unroll = 1; // TODO
const char *delim = "\t";

struct proc_threadcounts *rbuf;
pid_t pid;
static void configure_rdtsc() {
}

static int countsize;
static uint64_t target_tid;
static void init_rdtsc() {
    countsize = sizeof(struct proc_threadcounts) + 2 * sizeof(struct proc_threadcounts_data);
    rbuf = (struct proc_threadcounts*)malloc(countsize);
	 pid = getpid();
	 pthread_threadid_np(pthread_self(), &target_tid);
}

static unsigned long long int rdtsc() {
	proc_pidinfo(pid, PROC_PIDTHREADCOUNTS, target_tid, rbuf, countsize);
	struct proc_threadcounts_data *p = &(rbuf->ptc_counts[1]);
	uint64_t cycle = p->ptcd_cycles;
	return cycle;
}

static void shuffle(int *array, size_t n) {
  if (n > 1) {
    size_t i;
    for (i = 0; i < n - 1; i++) {
      size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
      int t = array[j];
      array[j] = array[i];
      array[i] = t;
    }
  }
}

static void init_dbufs(uint64_t **out_data1, uint64_t **out_data2) {
  // Initialize two 256MB data buffers, with the same linked-list
  // of offsets.
  size_t size = 256 * 1024 * 1024;
  size_t cache_line_size = 64;
  size_t count = size / cache_line_size;
  size_t stride = cache_line_size / sizeof(void *);
  int *numbers = malloc(count * sizeof(int));
  for (int i = 0; i < count; i++) {
    numbers[i] = i;
  }
  shuffle(numbers, count);

  uint64_t *data1 = calloc(size, 1);
  uint64_t *data2 = (uint64_t *)((char *)calloc(size + 64, 1) + 64);
  int next = numbers[count - 1];
  for (int i = 0; i < count; i++) {
    int n = numbers[i];
    data1[stride * n] = next * stride;
    data2[stride * n] = next * stride;
    next = n;
  }

  *out_data1 = data1;
  *out_data2 = data2;
  free(numbers);
}

static int add_prep(uint32_t *ibuf, int instr_type) {
  int o = 0;

  // free as much of the prf as possible
  switch (instr_type) {
  case 4: // gpr prf size
    for (int i = 5; i < 31; i++)
      ibuf[o++] = 0xd2800000 | i; // mov xi, #0
    break;

  case 5: // simd/fp prf size
    for (int i = 0; i < 32; i++)
      ibuf[o++] = 0x4ea11c20 | i; // mov.16b vi, v1
    break;
  case 15:
      {
          ibuf[o++] = 0xaa0003fa; // mov x26, x0
          ibuf[o++] = 0xaa0203fb; // mov x27, x2
          ibuf[o++] = 0xaa0403fc; // mov x28, x4
          ibuf[o++] = 0xaa0303fd; // mov x29, x3
          ibuf[o++] = 0xaa0103fe; // mov x30, x1
          ibuf[o++] = 0xaa0303f1; // mov x17, x3
          break;
      }
  }

  return o;
}
static int add_filler(uint32_t *ibuf, int instr_type, int j) {
  int o = 0;

  // "spike" is used to mean the first icount where the minimum time had clearly
  // jumped up

  switch (instr_type) {
  case 0: // OOO window maximum size (firestorm spike at 2295, icestorm spike at 415)
    ibuf[o++] = 0xd503201f; // nop
    break;
  case 1: // maximum in flight renames (firestorm spike at 623, icestorm spike at 111)
    ibuf[o++] = 0xd2800005; // mov x5, 0
    break;
  case 2: // load buffer size (firestorm spike at 129, icestorm spike at 29 (?))
    ibuf[o++] = 0xf9400045; // ldr x5, [x2]
    break;
  case 3: // store buffer size (firestorm spike at 108, icestorm spike at 36 (?))
    ibuf[o++] = 0xf9000445; // str x5, [x2, #8]
    break;
  case 4: // gpr prf size (firestorm spike at 380, icestorm spike at 79)
    ibuf[o++] = 0x8b0b0165; // add x5, x11, x11
    break;
  case 5: // simd/fp prf size (firestorm spike at 434, icestorm spike at 87)
    ibuf[o++] = 0x4e228420; // add v0.16b, v1.16b, v2.16b
    break;
  case 6: // scheduler (rs) size (firestorm spike at 158, icestorm spike at 34)
    ibuf[o++] = 0x8b010005; // add x5, x0, x1 (depends on pending load)
    break;
  case 7: // untaken branches (firestorm spike at 144, icestorm spike at 32)
    if (j == 0)
      ibuf[o++] = 0xeb0500bf; // cmp	x5, x5
    ibuf[o++] = 0x54000781;   // b.ne	.+0xF0
    break;
  case 8: // confused attempt to get a reoder buffer size (firestorm spike at 853)
    if (j == 0) {
      ibuf[o++] = 0xeb0500bf; // cmp	x5, x5
    } else if (j - 1 < 100) {
      ibuf[o++] = 0xf9000445; // str x5, [x2, #8]
    } else if (j - 1 - 100 < 130) {
      ibuf[o++] = 0x54000781; // b.ne	.+0xF0
    } else {
      ibuf[o++] = 0xd2800005; // mov x5, 0
    }
    break;
  case 9: // calls in flight (firestorm spike at 30, icestorm spike at 11)
    ibuf[o++] = 0x94000002; // bl +8
    ibuf[o++] = 0x14000002; // b  +8
    ibuf[o++] = 0xd65f03c0; // ret
    break;
  case 10: // uncond branch (firestorm spike at 88, icestorm spike at 32)
    ibuf[o++] = 0x14000001; // b  +4
    break;
  case 11: // taken branch (firestorm spike at 88, icestorm spike at 32)
    if (j == 0)
      ibuf[o++] = 0xeb0500bf; // cmp x5, x5
    ibuf[o++] = 0x54000020; // b.eq .+4
    break;
  case 12: // not-taken compare+branch (firestorm spike at 129)
    ibuf[o++] = 0xeb0500bf; // cmp x5, x5
    ibuf[o++] = 0x54000021; // b.ne .+4
    break;
  case 13: // taken compare+branch (firestorm spike at 88)
    ibuf[o++] = 0xeb0500bf; // cmp  x5, x5
    ibuf[o++] = 0x54000020; // b.eq .+4
    break;
  case 14: // confused attempt to get a reoder buffer size (firestorm spike at 853)
    if (j % 3 == 0) {
      ibuf[o++] = 0x8B0B0165; // ADD x0, x5, x5
    } else if (j % 3 == 1) {
      ibuf[o++] = 0x1E60C000; // FABS d0, d0
    } else {
        ibuf[o++] = 0xf9000445; // STR x5 [x2]
    }
    break;
  case 15:
    ibuf[o++] = 0xf90007a5; // strb w5, [x29, #8]
    break;
  }

  return o;
}

void make_routine(uint32_t *ibuf, int icount, int instr_type) {
  int o = 0;
  mprotect(ibuf, 0x400000, PROT_WRITE);
  // prologue
  if (instr_type == 5) {
    ibuf[o++] = 0x6dbb3bef; // stp	d15, d14, [sp, #-80]!
    ibuf[o++] = 0x6d0133ed; // stp	d13, d12, [sp, #16]
    ibuf[o++] = 0x6d022beb; // stp	d11, d10, [sp, #32]
    ibuf[o++] = 0x6d0323e9; // stp	d9, d8, [sp, #48]
    ibuf[o++] = 0xa9047bfd; // stp	x29, x30, [sp, #64]
  } else {
    ibuf[o++] = 0xa9b87bfd; // stp	x29, x30, [sp, #-128]!
    ibuf[o++] = 0xa9016ffc; // stp	x28, x27, [sp, #16]
    ibuf[o++] = 0xa90267fa; // stp	x26, x25, [sp, #32]
    ibuf[o++] = 0xa9035ff8; // stp	x24, x23, [sp, #48]
    ibuf[o++] = 0xa90457f6; // stp	x22, x21, [sp, #64]
    ibuf[o++] = 0xa9054ff4; // stp	x20, x19, [sp, #80]
    ibuf[o++] = 0xa90647f2; // stp	x18, x17, [sp, #96]
    ibuf[o++] = 0xa9073ff0; // stp	x16, x15, [sp, #112]
  }
        

  // next, next, data1, data2, its
  // x0 = offset into data1
  // x1 = offset into data2
  // x2 = data1
  // x3 = data2
  // x4 = its

  o += add_prep(ibuf + o, instr_type);

  int start = o;
  int load_count = 1;
  for (int i = 0; i < load_count; i++)
      if(instr_type == 15) ibuf[o++] = 0xf87a7b7a; // ldr x26, [x27, x26, lsl #3]
      else ibuf[o++] = 0xf8607840; // ldr	x0, [x2, x0, lsl #3]

  for (int j = 0; j < icount; j++) {
    o += add_filler(ibuf + o, instr_type, j);
  }

  for (int i = 0; i < load_count; i++)
    if(instr_type == 15) ibuf[o++] = 0xf87e7bbe; // ldr x30, [x29, x30, lsl #3]
    else ibuf[o++] = 0xf8617861; // ldr	x1, [x3, x1, lsl #3]

  // lfence mode?
  ibuf[o++] = 0xD5033B9F; // DSB ISH
  ibuf[o++] = 0xD5033FDF; // ISB

  // loop back to top
  ibuf[o++] = 0x71000484; // subs	w4, w4, #1
  int off = start - o;
  assert(off < 0 && off > -0x40000);
  ibuf[o++] = 0x54000001 | ((off & 0x7ffff) << 5); // b.ne

  if(instr_type == 15) ibuf[o++] = 0xaa1a03e0; // mov x0, x26
  // epilogue
  if (instr_type == 5) {
    ibuf[o++] = 0xa9447bfd; // ldp	x29, x30, [sp, #64]
    ibuf[o++] = 0x6d4323e9; // ldp	d9, d8, [sp, #48]
    ibuf[o++] = 0x6d422beb; // ldp	d11, d10, [sp, #32]
    ibuf[o++] = 0x6d4133ed; // ldp	d13, d12, [sp, #16]
    ibuf[o++] = 0x6cc53bef; // ldp	d15, d14, [sp], #80
    ibuf[o++] = 0xd65f03c0; // ret
  } else {
    ibuf[o++] = 0xa9473ff0; // ldp	x16, x15, [sp, #112]
    ibuf[o++] = 0xa94647f2; // ldp	x18, x17, [sp, #96]
    ibuf[o++] = 0xa9454ff4; // ldp	x20, x19, [sp, #80]
    ibuf[o++] = 0xa94457f6; // ldp	x22, x21, [sp, #64]
    ibuf[o++] = 0xa9435ff8; // ldp	x24, x23, [sp, #48]
    ibuf[o++] = 0xa94267fa; // ldp	x26, x25, [sp, #32]
    ibuf[o++] = 0xa9416ffc; // ldp	x28, x27, [sp, #16]
    ibuf[o++] = 0xa8c87bfd; // ldp	x29, x30, [sp], #128
    ibuf[o++] = 0xd65f03c0; // ret
  }

  mprotect(ibuf, 0x400000, PROT_WRITE);
  sys_icache_invalidate(ibuf, o * 4);
}

int test_entry_zero() {
  int test_high_perf_cores = 1;
  int instr_type = 1;
  int start_icount = 30;
  int stop_icount = 150;
  int stride_icount = 1;

  if (test_high_perf_cores) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  } else {
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
  }

  init_rdtsc();
  // Warm up cache and predictor
  volatile uint64_t warmup1 = rdtsc();
  warmup1 = rdtsc();
  warmup1 = rdtsc();
  warmup1 = rdtsc();
  warmup1 = rdtsc();
  warmup1 = rdtsc();
    
    __asm__ volatile("isb");
    volatile uint64_t cycle_start = rdtsc();
    __asm__ volatile("isb");
    __asm__ volatile("isb");
    volatile uint64_t cycle_end = rdtsc();
    __asm__ volatile("isb");

    

  printf("base cycle: %llu\n", cycle_end - cycle_start);

    while(1) {
        __asm__ volatile("isb");
        volatile uint64_t cycle_start = rdtsc();
        __asm__ volatile("isb");

        // test width
        int loop_count = 30000000;
        for(int i = 0; i < loop_count; i++) {
            __asm__ volatile (".fill 1344, 4, 0xd503201f");
        }

        __asm__ volatile("isb");
        volatile uint64_t cycle_end = rdtsc();
        __asm__ volatile("isb");
        uint64_t cycle = cycle_end - cycle_start;
//        printf("cycle: %llu\n", cycle);
        printf("IPC: %f\n", 1344.0 / ((double)cycle / (double)loop_count));
        sleep(1);
    }
}

int data[16][2] = {
    {415, 2295},
    {111, 623},
    {29, 129},
    {36, 108},
    {79, 380},
    {87, 434},
    {34, 158},
    {32, 144},
    {-1, 853},
    {11, 30},
    {32, 88},
    {32, 88},
    {-1, 129},
    {-1, 88},
    {-1, 853},
    {-1, 125}
};

int getValueForPair(int instr_type, int test_high_perf_cores) {
    return data[instr_type][test_high_perf_cores];
}



int test_entry(int instr_type, int test_high_perf_cores, int start_icount, int stop_icount, const char* basePath) {
  // int test_high_perf_cores = 1;
  // int instr_type = 1;
  // int start_icount = 1800;
  // int stop_icount = 2500;
  int stride_icount = 1;

  if (test_high_perf_cores) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  } else {
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
  }

  init_rdtsc();
  // Warm up cache and predictor
  volatile uint64_t warmup1 = rdtsc();
  warmup1 = rdtsc();
  warmup1 = rdtsc();
  warmup1 = rdtsc();
  warmup1 = rdtsc();
  warmup1 = rdtsc();
    
    __asm__ volatile("isb");
    volatile uint64_t cycle_start = rdtsc();
    __asm__ volatile("isb");
    __asm__ volatile("isb");
    volatile uint64_t cycle_end = rdtsc();
    __asm__ volatile("isb");

    
    
   uint64_t *data1, *data2;
   init_dbufs(&data1, &data2);
 
   void *mapping = mmap(NULL, 0x400000, PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
   uint32_t *ibuf = (uint32_t *)mapping;
 
   uint64_t next = 0;

   char filename[256];  // Buffer for filename
   snprintf(filename, sizeof(filename), "%s/output_%d_%d_%d_%d.tsv", basePath, instr_type, test_high_perf_cores, start_icount, stop_icount);
   FILE *file = fopen(filename, "w");
    if (!file) {
        perror("Error opening file for writing");
        return 1;  // Return with error code
    }

 
   for (int icount = start_icount; icount <= stop_icount; icount += stride_icount) {
     make_routine(ibuf, icount, instr_type);
 
     uint64_t (*routine)(uint64_t, uint64_t, uint64_t *, uint64_t *, uint64_t) = (void *)ibuf;
 
     uint64_t min_diff = 0x7fffffffffffffffLL;
     uint64_t max_diff = 0x0;
     uint64_t sum_diff = 0;
       
     mprotect(ibuf, 0x400000, PROT_EXEC);
     next = routine(next, next, data1, data2, its);
 
     for (int i = 0; i < outer_its; i++) {
 
       // in case we were on the wrong core earlier
       configure_rdtsc();
       long long start = rdtsc();
       next = routine(next, next, data1, data2, its);
       long long stop = rdtsc();
 
       uint64_t cycles = stop - start;
 
       sum_diff += cycles;
       if (min_diff > cycles) {
         min_diff = cycles;
       } 
       if (max_diff < cycles) {
         max_diff = cycles;
       }
     }
 
     printf("%d%s%.2f%s%.2f%s%.2f\n", icount, delim,
            1.0 * min_diff / its / unroll, delim,
            1.0 * sum_diff / its / unroll / outer_its, delim,
            1.0 * max_diff / its / unroll);
    fprintf(file, "%d%s%.2f%s%.2f%s%.2f\n", icount, delim,
      1.0 * min_diff / its / unroll, delim,
      1.0 * sum_diff / its / unroll / outer_its, delim,
      1.0 * max_diff / its / unroll);
   }

   fclose(file);  // Close the file

  return 0;
}

int test_everything(const char *path) {
  int instr_type, test_high_perf_cores;
    for (instr_type = 0; instr_type <= 15; instr_type++) {
        for (test_high_perf_cores = 0; test_high_perf_cores <= 1; test_high_perf_cores++) {
            int value = getValueForPair(instr_type, test_high_perf_cores);
            if(value == -1) continue;
            int start_icount = 0.5 * value;
            int stop_icount = 1.5 * value;
            printf("instr_type: %d, test_high_perf_cores: %d, start_icount: %d, stop_icount: %d\n", 
            instr_type, test_high_perf_cores, start_icount, stop_icount);

            test_entry(instr_type, test_high_perf_cores, start_icount, stop_icount, path);
            printf("======\n");
        }
    }
    return 0;
}

int test_entry_manual(const char* basePath) {

  test_entry(0, 1, 50, 1000, basePath);
  return 0;
}


