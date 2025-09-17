
#define _GNU_SOURCE
#define __USE_GNU 1

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ull unsigned long long

const ull TOP_N = 10; // How many top URLs we want to find

// SEGMENT_BLOCK_SIZE defines a small block which we're going to read to find
// real offsets It should be pretty small to avoid extra reads
// For good performance it should be aligned to 512 bytes or 4KB (depending on
// the storage)
// const ull SEGMENT_BLOCK_SIZE = 64;
const ull SEGMENT_BLOCK_SIZE = 512;

// SEGMENT_SIZE decrlare a sharded segments which we're going to read
// sequentially It should be pretty large to saturate bandwidth of NVM'e and
// disks, more than Here we expected that segment_size more than a single row
// const ull SEGMENT_SIZE = 1024 * 8; // 8KB
const ull SEGMENT_SIZE = 1024 * 1024; // 1MB

const ull QUEUE_DEPTH =
    16; // This number defines how many requests we can have in flight

// READ_FLAGS identifies how we're going to open the file
// O_DIRECT should help to avoid double caching in page cache and FS cache
// O_RDONLY because we don't need to write anything
const int READ_FLAGS = O_RDONLY | O_DIRECT;

// This is helper structure which describes a subset of rows in the file to be
// processed seek - offset in the file len - length of the segment Note that
// len can be larger than SEGMENT_SIZE because we need to read until \n to
// avoid splitting rows
typedef struct {
  size_t seek;
  size_t len;
} segment;

// This structure describes a map of segments
// data - array of segments
// count - number of segments
typedef struct {
  segment *data;
  size_t count;
} segment_map;

// This is a dumb heap which stores TOP_N URLs and their counts
typedef struct {
  ull *counts;
  char **urls;
  size_t size;
  size_t max;
} dumb_heap;

// This structure is used to lock segments during processing
// mu - mutex to lock the segment
// counter - is index pointer to the next segment to be processed
typedef struct {
  pthread_mutex_t mu; // mutex to lock the segment_map
  size_t counter;     // index of the next segment to be processed
  char *filename;     // the name of the file to be processed
  segment_map *map;   // a pointer to the segment_map
  dumb_heap *heap;    // a heap for result
} args;

// Just a helper function for debugging purposes
void sg_print(segment_map *m, char *filename) {
#define ROW_SIZE 40
  char buf[ROW_SIZE];

  if (m == NULL) {
    return;
  }

  int fd = open(filename, READ_FLAGS);
  if (fd == -1) {
    perror("error opening file");
    return;
  }

  for (int i = 0; i < m->count; i++) {
    segment *s = &m->data[i];

    lseek(fd, s->seek, SEEK_SET);
    size_t read_n = read(fd, buf, ROW_SIZE);
    printf("[%d] = {%zu, %zu} -> %s ...\n", i, s->seek, s->len, buf);
  }

  close(fd);
}

// The main purpose of the method is to create a sparse map of file and create
// segments which can be read in parallel Because we don't have fixed structre
// we'll split file to multiple similar sized blocks and try to find real
// offsets
segment_map *sg_create(char *filename) {
  assert(SEGMENT_SIZE >= 4 * SEGMENT_BLOCK_SIZE);

  segment_map *map = malloc(sizeof(segment_map));

  struct stat sb;
  if (stat(filename, &sb) == -1) {
    perror("stat");
    return NULL;
  }

  // printf("File size: %lld bytes\n", (unsigned long long) sb.st_size);
  map->count = sb.st_size / SEGMENT_SIZE + 1;
  map->data = malloc(map->count * sizeof(segment));
  // printf("Segment count: %zu\n", map->count);

  int fd = open(filename, READ_FLAGS);
  if (fd == -1) {
    perror("error opening file");
    return NULL;
  }

  size_t seeked = 0;
  size_t previous = 0;

  char *buf = malloc(SEGMENT_BLOCK_SIZE * sizeof(char));
  for (int i = 0; i < map->count; i++) {
    segment *s = &map->data[i];
    s->seek = previous;
    s->len = SEGMENT_SIZE;

    lseek(fd, s->seek + SEGMENT_SIZE, SEEK_SET);
    size_t n = read(fd, buf, SEGMENT_BLOCK_SIZE);
    if (n != -1) {
      char *newline = strstr(buf, "\n");
      // Exceptional case where row is larger than SEGMENT_SIZE
      assert(newline != NULL);
      size_t local_offset = newline - buf; // include \n
      s->len = s->len + local_offset + 1;
      previous = s->seek + s->len;
    } else {
      // probably we reached EOF
      // there is no need to work with last \n
      s->len = n;
      previous = s->seek + s->len;
      break;
    }
  }
  // sg_print(map, filename);
  free(buf);

  close(fd);

  return map;
}

void sg_free(segment_map *m) {
  free(m->data);
  free(m);
}

int acquire_segment(args *arguments) {
  pthread_mutex_lock(&arguments->mu);
  int idx = arguments->counter;
  if (idx < arguments->map->count) {
    arguments->counter++;
  } else {
    idx = -1; // no more segments
  }
  pthread_mutex_unlock(&arguments->mu);
  return idx;
}

dumb_heap *dbheap_create(size_t max) {
  dumb_heap *h = malloc(sizeof(dumb_heap));
  h->counts = malloc(max * sizeof(ull));
  h->urls = malloc(max * sizeof(char *));
  for (size_t i = 0; i < max; i++) {
    h->urls[i] = malloc(SEGMENT_BLOCK_SIZE * sizeof(char)); // TODO 2048
  }
  h->size = 0;
  h->max = max;
  return h;
}

dumb_heap *dbheap_free(dumb_heap *h) {
  free(h->counts);
  for (size_t i = 0; i < h->size; i++) {
    free(h->urls[i]);
  }
  free(h->urls);
  free(h);
  return NULL;
}

dumb_heap *dbheap_add(dumb_heap *h, char *url, ull count) {
  if (h->size < h->max) {
    h->counts[h->size] = count;
    h->urls[h->size] = strdup(url);
    h->size++;
  } else {
    // find minimum
    // here is a good place to optimize
    // probably with vectorization or auto-vectorization
    size_t min_idx = 0;
    for (size_t i = 1; i < h->size; i++) {
      if (h->counts[i] < h->counts[min_idx]) {
        min_idx = i;
      }
    }
    if (count > h->counts[min_idx]) {
      // here we can optimize memory allocations
      free(h->urls[min_idx]);
      h->urls[min_idx] = strdup(url);
      h->counts[min_idx] = count;
    }
  }
  return h;
}

dumb_heap *dbheap_merge(dumb_heap *h, dumb_heap *other) {
  for (size_t i = 0; i < other->size; i++) {
    dbheap_add(h, other->urls[i], other->counts[i]);
  }
  return h;
};

dumb_heap *dbheap_print(dumb_heap *h) {
  // print in descending order
  for (size_t i = 0; i < h->size; i++) {
    size_t max_idx = 0;
    for (size_t j = 1; j < h->size; j++) {
      if (h->counts[j] > h->counts[max_idx]) {
        max_idx = j;
      }
    }
    // debug output with count
    // printf("%s %llu\n", h->urls[max_idx], h->counts[max_idx]);
    printf("%s\n", h->urls[max_idx]);
    h->counts[max_idx] = 0; // mark as printed
  }
  return h;
};

// This thread / function reads multiple segment in a loop and applies all new
// rows to already processed data in this thread Later we can merge results from
// multiple threads I intentionally don't implement heap here to because for
// small top-N it will be slower
void *thread_main(void *arguments) {
  args *a = (args *)arguments;

  char buf[SEGMENT_SIZE + SEGMENT_BLOCK_SIZE];

  dumb_heap *h = dbheap_create(TOP_N);
  while (1) {
    // Let's erase buffer to avoid mess between reads
    // Perhaps we can optimize it later because we always knows how many bytes
    // we read
    memset(buf, 0, SEGMENT_SIZE + SEGMENT_BLOCK_SIZE);

    int idx = acquire_segment(a);
    if (idx == -1) {
      break; // no more segments
    }

    segment *s = &a->map->data[idx];
    // printf("Segment %d: seek=%zu, len=%zu\n", idx, s->seek, s->len);
    int fd = open(a->filename, READ_FLAGS);
    if (fd == -1) {
      perror("error opening file");
      return NULL;
    }
    lseek(fd, s->seek, SEEK_SET);
    size_t read_n = read(fd, buf, s->len);
    close(fd);
    // printf("Read %zu bytes of len: %zu\n", read_n, s->len);


    char url[SEGMENT_BLOCK_SIZE]; // 2048?
    ull c = 0;
    memset(url, 0, sizeof(url));
    for (int i = 0, j = 0, k = 0; i < read_n; i++) {
      switch (buf[i]) {
      case ' ':
        // TODO: probably we can omit creating a new string here.
        // We can propagate pointers to dumb_heap directly and then heap can
        // decide to copy or not
        strncpy(url, &buf[j], i - j);
        url[i - j] = '\0'; // this hint allows to not override the full cstring
        // printf("url: >%s<\t", url);
        k = i + 1;
        break;
      case '\0':
      case '\n':
        // This is the hotpath, good option to speedup
        c = strtoull(&buf[k], NULL, 10); // 1-thread 1.28 + 0.23
        // printf("count: >%s<\n", &buf[k]);
        dbheap_add(h, url, c);
        // memset(url, 0, sizeof(url));
        j = i + 1;
        break;
      }
    }
  }

  // Merge thread-local heap to result heap
  pthread_mutex_lock(&(a->mu));
  dbheap_merge(a->heap, h);
  pthread_mutex_unlock(&(a->mu));

  dbheap_free(h);
  // printf("Thread %d finished\n", tid);

  return NULL;
}

int main() {
  char path[512];
  if (fgets(path, sizeof(path), stdin) == NULL) {
    perror("Expected to get abspath to file");
    return 1;
  }
  // strip newline
  char *newline = strstr(path, "\n");
  if (newline != NULL) {
    *newline = '\0';
  }

  // This code skims the file and creates a map of segments
  // Even for large files it should be pretty fast
  // After that we can read segments in parallel
  segment_map *m = sg_create(path);
  if (m == NULL) {
    perror("Failed to create map");
    exit(1);
  }
  int queue_depth = QUEUE_DEPTH;

  volatile size_t counter = 0;

  // Yes, looks like it should be in dedicated method-constructor
  args arguments;
  arguments.mu = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
  arguments.counter = 0;
  arguments.map = m;
  arguments.filename = path;
  arguments.heap = dbheap_create(TOP_N);

  pthread_t threads[queue_depth];
  for (int i = 0; i < queue_depth; i++) {
    if (pthread_create(&threads[i], NULL, thread_main, &arguments) != 0) {
      perror("Failed to create thread");
      exit(1);
    }
  }

  // Now let's wait all other threads and merge heaps into one.
  for (int i = 0; i < queue_depth; i++) {
    if (pthread_join(threads[i], NULL) != 0) {
      perror("Failed to join thread");
      exit(1);
    }
  }

  dbheap_print(arguments.heap);

  sg_free(m);
  dbheap_free(arguments.heap);

  return 0;
}