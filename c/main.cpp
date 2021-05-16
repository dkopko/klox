#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cb_integration.h"
#include "common.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

static void repl() {
  char line[1024];
  for (;;) {
    printf("> ");

    if (!fgets(line, sizeof(line), stdin)) {
      printf("\n");
      break;
    }

    interpret(line);
  }
}

static bool needs_gc_deinit = false;

void
exitWithError(int exitCode) {
  if (needs_gc_deinit)
    gc_deinit();
  exit(exitCode);
}


static char* readFile(const char* path) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Could not open file \"%s\".\n", path);
    exitWithError(74);
  }

  fseek(file, 0L, SEEK_END);
  size_t fileSize = ftell(file);
  rewind(file);

  char* buffer = (char*)malloc(fileSize + 1);
  if (buffer == NULL) {
    fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
    exitWithError(74);
  }

  size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
  if (bytesRead < fileSize) {
    fprintf(stderr, "Could not read file \"%s\".\n", path);
    exitWithError(74);
  }

  buffer[bytesRead] = '\0';

  fclose(file);
  return buffer;
}

static void runFile(const char* path) {
  char* source = readFile(path);
  InterpretResult result = interpret(source);
  free(source); // [owner]

  if (result == INTERPRET_COMPILE_ERROR) exitWithError(65);
  if (result == INTERPRET_RUNTIME_ERROR) exitWithError(70);
}

int main(int argc, const char* argv[]) {
  /* Make sure main thread is marked. */
  on_main_thread = true;
  can_print      = true;

  struct cb_params cb_params = CB_PARAMS_DEFAULT;
  int ret;

  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  /* Initialize cb library. */
  ret = cb_module_init();
  if (ret != 0) {
    fprintf(stderr, "cb_module_init() failed.\n");
    return EXIT_FAILURE;
  }

  /* Create thread-local continuous buffer. */
  uintmax_t ring_size = 1 << 12;  //Start very small by default, 1 page = 4096 bytes
  if (getenv("KLOX_RING_SIZE")) sscanf(getenv("KLOX_RING_SIZE"), "%ju", &ring_size);
  cb_params.ring_size = (size_t)ring_size;
  //cb_params.mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE; //FIXME doesn't work (klox_debug suite failures)
  //cb_params.mmap_flags = MAP_PRIVATE | MAP_POPULATE; //FIXME doesn't work (klox_debug suite failures)
  //cb_params.mmap_flags = MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE; //FIXME doesn't work (klox_debug suite failures)
  cb_params.mmap_flags = MAP_SHARED | MAP_POPULATE;
  cb_params.flags |= CB_PARAMS_F_MLOCK;
  cb_params.on_resize = &klox_on_cb_resize;
  thread_cb = cb_create(&cb_params, sizeof(cb_params));
  if (!thread_cb) {
    fprintf(stderr, "Could not create continuous buffer. \n");
    return EXIT_FAILURE;
  }
  thread_ring_start = cb_ring_start(thread_cb);
  thread_ring_mask = cb_ring_mask(thread_cb);

  /* Make one allocation to preserve CB_NULL == (cb_offset_t)0. */
  {
    cb_offset_t new_offset;
    cb_memalign(&thread_cb, &new_offset, 1, 1);
    assert(new_offset == CB_NULL);
  }

  /* Create thread-local continuous buffer region. */
  ret = logged_region_create(&thread_cb, &thread_region, 1, 1024 * 1024, 0);
  if (ret != CB_SUCCESS)
  {
      fprintf(stderr, "Could not create region.\n");
      return EXIT_FAILURE;
  }

  /* Initialize CB-based GC. */
  ret = gc_init();
  if (ret != 0) {
      fprintf(stderr, "Could not create GC structures.\n");
      return EXIT_FAILURE;
  }
  needs_gc_deinit = true;

  initVM();

  if (argc == 1) {
    repl();
  } else if (argc == 2) {
    runFile(argv[1]);
  } else {
    fprintf(stderr, "Usage: klox [path]\n");
    exitWithError(64);
  }

  freeVM();
  gc_deinit();
  return 0;
}
