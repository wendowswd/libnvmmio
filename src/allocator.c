#define _GNU_SOURCE

#include <fcntl.h>
#include <libpmem.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <stdbool.h>

#include "allocator.h"
#include "internal.h"
#include "debug.h"

#define DIR_PATH "%s/.libnvmmio-%lu"
#define DATA_PATH "%s/.libnvmmio-%lu/data-%d.log"
#define ENTRIES_PATH "%s/.libnvmmio-%lu/entries.log"
#define UMAS_PATH "%s/.libnvmmio-%lu/umas.log"

typedef struct freelist_struct {
  list_node_t *head;
  unsigned long count;
  pthread_mutex_t mutex;
} freelist_t;

static pthread_t background_table_alloc_thread;
static pthread_cond_t background_table_alloc_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t background_table_alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int background_table_alloc = false;

static freelist_t *global_tables_list = NULL;
static freelist_t *global_entries_list = NULL;
static freelist_t *global_data_list[NR_LOG_SIZES] = {NULL, };
static freelist_t *global_uma_list = NULL;

static __thread freelist_t *local_tables_list = NULL;
static __thread freelist_t *local_entries_list = NULL;
static __thread freelist_t *local_data_list[NR_LOG_SIZES] = {NULL, };
static __thread list_node_t *local_node_head = NULL;

static int umaid = -1;
static char *pmem_path;
static unsigned long libnvmmio_pid;

static inline int get_uma_id(void) {
  int old, new;

  /* TODO: It is necessary to check whether the counter does not exceed the
   * INT_MAX */
  do {
    old = umaid;
    new = old + 1;
  } while (!__sync_bool_compare_and_swap(&umaid, old, new));

  return new;
}

static void rmlogs(char *path) {
	DIR * dir_ptr = NULL;
	struct dirent *file = NULL;
	char filename[1024];
	int s;

	dir_ptr = opendir(path);
	if (__glibc_unlikely(dir_ptr == NULL)) {
		handle_error("opendir");
	}

	while ((file = readdir(dir_ptr)) != NULL) {
		if (strcmp(file->d_name, ".") == 0 || strcmp(file->d_name, "..") == 0) {
			continue;
		}

		sprintf(filename, "%s/%s", path, file->d_name);

		s = unlink(filename);
		if (__glibc_unlikely(s != 0)) {
			handle_error("unlink");
		}
	}

	s = closedir(dir_ptr);
	if (__glibc_unlikely(s != 0)) {
		handle_error("closedir");
	}

	s = rmdir(path);
	if (__glibc_unlikely(s != 0)) {
		handle_error("rmdir");
	}
}


#ifdef _LIBNVMMIO_DEBUG
static char *size2str(size_t size, char *buf) {
  char unit[3];

  strcpy(unit, "B");
  if (size >= 1024) {
    size = size / 1024;
    strcpy(unit, "KB");
    if (size >= 1024) {
      size = size / 1024;
      strcpy(unit, "MB");
      if (size >= 1024) {
        size = size / 1024;
        strcpy(unit, "GB");
      }
    }
  }

  sprintf(buf, "%lu%s", size, unit);
  return buf;
}
#endif /* _LIBNVMMIO_DEBUG */

static void *map_logfile(const char *path, size_t len) {
  void *addr;
  int fd, s;
  int flags;
#ifdef _LIBNVMMIO_DEBUG
  char buf[10];
#endif /* LIBNVMMIO_DEBUG */

  if (path == NULL) {
    fd = -1;
    flags = MAP_ANONYMOUS | MAP_SHARED;
  } else {
    fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0777);
    if (__glibc_unlikely(fd == -1)) {
      handle_error("open");
    }

    s = posix_fallocate(fd, 0, len);
    if (__glibc_unlikely(s != 0)) {
      handle_error("fallocate");
    }
    flags = MAP_SHARED | MAP_POPULATE;
  }

  addr = mmap(0, len, PROT_READ | PROT_WRITE, flags, fd, 0);
  if (__glibc_unlikely(addr == MAP_FAILED)) {
    handle_error("mmap");
  }

	LIBNVMMIO_DEBUG("file:%s, size:%s", path, size2str(len, buf));

  return addr;
}

list_node_t *alloc_list_node(void) {
  list_node_t *node;

  if (local_node_head == NULL) {
    node = (list_node_t *)malloc(sizeof(list_node_t));
    if (__glibc_unlikely(node == NULL)) {
      handle_error("malloc");
    }
  } else {
    node = local_node_head;
    local_node_head = local_node_head->next;
  }
  node->next = NULL;
  return node;
}

static void free_node(list_node_t *node) {
  node->ptr = NULL;
  node->next = local_node_head;
  local_node_head = node;
}

static void init_entries_lock(list_node_t *head) {
  log_entry_t *entry;

  while (head) {
    entry = (log_entry_t *)(head->ptr);
    entry->rwlockp = (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t));
    if (__glibc_unlikely(entry->rwlockp == NULL)) {
      handle_error("malloc for rwlockp");
    }
    head = head->next;
  }
}

static list_node_t *create_list(void *address, size_t size, unsigned long count,
                                list_node_t **tail) {
  list_node_t *node;
  list_node_t *head = NULL;
  unsigned long i;

  if (tail != NULL) {
    *tail = NULL;
  }

  for (i = 0; i < count; i++) {
    node = alloc_list_node();
    node->ptr = address + (i * size);
    node->next = head;
    head = node;

    if (tail && *tail == NULL) {
      *tail = node;
    }
  }
  return head;
}

static void fill_global_tables_list(int lock) {
  list_node_t *head;
  list_node_t *tail;
  size_t total_size;
  void *address;

  total_size = MAX_FREE_NODES * sizeof(log_table_t);
  address = map_logfile(NULL, total_size);
  head = create_list(address, sizeof(log_table_t), MAX_FREE_NODES, &tail);

  if (lock) {
    pthread_mutex_lock(&global_tables_list->mutex);
  }

  tail->next = global_tables_list->head;
  global_tables_list->head = head;
  global_tables_list->count += MAX_FREE_NODES;

  if (lock) {
    pthread_mutex_unlock(&global_tables_list->mutex);
  }
}

static void *background_table_alloc_thread_func(__attribute__((unused))void *parm) {
  int s;
  LIBNVMMIO_DEBUG("table_alloc_thread start on %d", sched_getcpu());

  while (true) {
    s = pthread_mutex_lock(&background_table_alloc_mutex);
    if (__glibc_unlikely(s != 0)) {
      handle_error("pthread_mutex_lock");
    }

    while (background_table_alloc == false) {
      s = pthread_cond_wait(&background_table_alloc_cond,
                            &background_table_alloc_mutex);
      if (__glibc_unlikely(s != 0)) {
        handle_error("pthread_cond_wait");
      }
    }
    LIBNVMMIO_DEBUG("wake up!!");
    fill_global_tables_list(true);
    background_table_alloc = false;

    s = pthread_mutex_unlock(&background_table_alloc_mutex);
    if (__glibc_unlikely(s != 0)) {
      handle_error("pthread_mutex_unlock");
    }
  }
  return NULL;
}

void exit_background_table_alloc_thread(void) {
  int s;
  s = pthread_cancel(background_table_alloc_thread);
  if (__glibc_unlikely(s != 0)) {
    handle_error_en(s, "pthread_cancel");
  }
}

static void create_global_tables_list(int count) {
  void *address;
  size_t total_size;
  int s;

  if (global_tables_list == NULL) {
    global_tables_list = (freelist_t *)malloc(sizeof(freelist_t));
    if (__glibc_unlikely(global_tables_list == NULL)) {
      handle_error("malloc");
    }
    pthread_mutex_init(&global_tables_list->mutex, NULL);
    total_size = count * sizeof(log_table_t);
    address = map_logfile(NULL, total_size);

    global_tables_list->head =
        create_list(address, sizeof(log_table_t), count, NULL);
    global_tables_list->count = count;

    /* background thread */
    s = pthread_create(&background_table_alloc_thread, NULL,
                       background_table_alloc_thread_func, NULL);
    if (__glibc_unlikely(s != 0)) {
      handle_error("pthread_create");
    }
  }
}

static void create_global_entries_list(size_t data_file_size) {
  void *address;
  unsigned long count;
  size_t entries_file_size;
  char filename[40];

  if (global_entries_list == NULL) {
    global_entries_list = (freelist_t *)malloc(sizeof(freelist_t));
    if (__glibc_unlikely(global_entries_list == NULL)) {
      handle_error("malloc");
    }
    pthread_mutex_init(&global_entries_list->mutex, NULL);
    count = data_file_size >> PAGE_SHIFT;
    entries_file_size = count * sizeof(log_entry_t);
    sprintf(filename, ENTRIES_PATH, pmem_path, libnvmmio_pid);
    address = map_logfile(filename, entries_file_size);
    global_entries_list->head =
        create_list(address, sizeof(log_entry_t), count, NULL);
    global_entries_list->count = count;
    init_entries_lock(global_entries_list->head);
  }
}

static void create_global_data_list(size_t data_file_size) {
  void *address;
  unsigned long count;
  size_t log_size;
  char filename[40];
  int i;

  for (i = 0; i < NR_LOG_SIZES; i++) {
    global_data_list[i] = (freelist_t *)malloc(sizeof(freelist_t));
    if (__glibc_unlikely(global_data_list[i] == NULL)) {
      handle_error("malloc");
    }
    pthread_mutex_init(&global_data_list[i]->mutex, NULL);

    sprintf(filename, DATA_PATH, pmem_path, libnvmmio_pid, i);
    address = map_logfile(filename, data_file_size);
    log_size = 1UL << LOG_SHIFT(i);
    count = data_file_size >> LOG_SHIFT(i);
    global_data_list[i]->head = create_list(address, log_size, count, NULL);
    global_data_list[i]->count = count;
  }
}

static void create_global_umas_list(void) {
  size_t len;
  void *addr;
  char filename[40];

  global_uma_list = (freelist_t *)malloc(sizeof(freelist_t));
  if (__glibc_unlikely(global_uma_list == NULL)) {
    handle_error("malloc");
  }
  pthread_mutex_init(&global_uma_list->mutex, NULL);
  len = MAX_NR_UMAS * sizeof(uma_t);
  sprintf(filename, UMAS_PATH, pmem_path, libnvmmio_pid);
  addr = map_logfile(filename, len);
  global_uma_list->head = create_list(addr, sizeof(uma_t), MAX_NR_UMAS, NULL);
  global_uma_list->count = MAX_NR_UMAS;
}

static inline void alloc_list(freelist_t **list) {
  if (*list == NULL) {
    *list = (freelist_t *)malloc(sizeof(freelist_t));

    if (__glibc_unlikely(*list == NULL)) {
      handle_error("malloc");
    }
    (*list)->head = NULL;
    (*list)->count = 0;
  }
}

static inline void alloc_local_entries_list(void) {
  if (local_entries_list == NULL) {
    local_entries_list = (freelist_t *)malloc(sizeof(freelist_t));

    if (__glibc_unlikely(local_entries_list == NULL)) {
      handle_error("malloc for local_entries_list");
    }
    local_entries_list->head = NULL;
    local_entries_list->count = 0;
  }
}

static inline void alloc_local_data_list(log_size_t log_size) {
  if (local_data_list[log_size] == NULL) {
    local_data_list[log_size] = (freelist_t *)malloc(sizeof(freelist_t));

    if (__glibc_unlikely(local_data_list[log_size] == NULL)) {
      handle_error("malloc for local_data_list");
    }
    local_data_list[log_size]->head = NULL;
    local_data_list[log_size]->count = 0;
  }
}

static void fill_local_tables_list(void) {
  list_node_t *node;
  unsigned long count, i;
  int s;

  s = pthread_mutex_lock(&global_tables_list->mutex);
  if (__glibc_unlikely(s != 0)) {
    handle_error("pthread_mutex_lock");
  }

  count = global_tables_list->count;

  if (count == 0) {
    fill_global_tables_list(false);
    count = global_tables_list->count;
  }

  if (count > NR_FILL_NODES) {
    count = NR_FILL_NODES;
  }

  node = global_tables_list->head;
  local_tables_list->head = node;

  for (i = 0; i < count - 1; i++) {
    node = node->next;
  }
  global_tables_list->head = node->next;
  node->next = NULL;

  global_tables_list->count -= count;
  local_tables_list->count += count;

  if (global_tables_list->count < MAX_FREE_NODES) {
    s = pthread_mutex_lock(&background_table_alloc_mutex);
    if (__glibc_unlikely(s != 0)) {
      handle_error("pthread_mutex_lock");
    }

    background_table_alloc = true;

    s = pthread_mutex_unlock(&background_table_alloc_mutex);
    if (__glibc_unlikely(s != 0)) {
      handle_error("pthread_mutex_unlock");
    }

    s = pthread_cond_signal(&background_table_alloc_cond);
    if (__glibc_unlikely(s != 0)) {
      handle_error("pthread_cond_signal");
    }
  }

  pthread_mutex_unlock(&global_tables_list->mutex);
}

static void fill_local_entries_list(void) {
  list_node_t *node;
  unsigned long nrnodes, i;

  pthread_mutex_lock(&global_entries_list->mutex);

  nrnodes = global_entries_list->count;

  if (__glibc_unlikely(nrnodes == 0)) {
    handle_error("global_entries_list does not have anything");
  }

  if (nrnodes > NR_FILL_NODES) {
    nrnodes = NR_FILL_NODES;
  }

  node = global_entries_list->head;
  local_entries_list->head = node;

  for (i = 0; i < nrnodes - 1; i++) {
    node = node->next;
  }
  global_entries_list->head = node->next;
  node->next = NULL;

  global_entries_list->count -= nrnodes;
  local_entries_list->count += nrnodes;

  pthread_mutex_unlock(&global_entries_list->mutex);
}

static void fill_local_data_list(log_size_t log_size) {
  list_node_t *node;
  unsigned long count, i;

  pthread_mutex_lock(&global_data_list[log_size]->mutex);

  count = global_data_list[log_size]->count;

  if (__glibc_unlikely(count == 0)) {
    handle_error("global_data_list does not have anything");
  }

  if (count > NR_FILL_NODES) {
    count = NR_FILL_NODES;
  }

  node = global_data_list[log_size]->head;
  local_data_list[log_size]->head = node;

  for (i = 0; i < count - 1; i++) {
    node = node->next;
  }
  global_data_list[log_size]->head = node->next;
  node->next = NULL;

  global_data_list[log_size]->count -= count;
  local_data_list[log_size]->count += count;

  pthread_mutex_unlock(&global_data_list[log_size]->mutex);
}

static void put_log_global(freelist_t *local_list, freelist_t *global_list,
                           int nrnodes) {
  list_node_t *node, *tmp_node;
  int i;

  node = local_list->head;
  for (i = 1; i < nrnodes; i++) {
    node = node->next;
  }

  pthread_mutex_lock(&global_list->mutex);

  tmp_node = local_list->head;
  local_list->head = node->next;
  node->next = global_list->head;
  global_list->head = tmp_node;
  global_list->count += nrnodes;
  local_list->count -= nrnodes;

  pthread_mutex_unlock(&global_list->mutex);
}

static void put_log_local(log_entry_t *entry, log_size_t log_size) {
  list_node_t *data_node, *entry_node;

  data_node = alloc_list_node();
  data_node->ptr = entry->data;

  if (local_data_list[log_size] == NULL) {
    alloc_local_data_list(log_size);
  }

  data_node->next = local_data_list[log_size]->head;
  local_data_list[log_size]->head = data_node;

  local_data_list[log_size]->count += 1;

  if (local_data_list[log_size]->count > MAX_FREE_NODES) {
    put_log_global(local_data_list[log_size], global_data_list[log_size],
                   NR_FILL_NODES);
  }

  entry_node = alloc_list_node();
  entry_node->ptr = entry;

  if (local_entries_list == NULL) {
    alloc_list(&local_entries_list);
  }

  entry_node->next = local_entries_list->head;
  local_entries_list->head = entry_node;

  local_entries_list->count += 1;

  if (local_entries_list->count > MAX_FREE_NODES) {
    /* GC */
    put_log_global(local_entries_list, global_entries_list, NR_FILL_NODES);
  }
}

uma_t *alloc_uma(void) {
  list_node_t *node;
  uma_t *uma;

  pthread_mutex_lock(&global_uma_list->mutex);
  node = global_uma_list->head;
  global_uma_list->head = node->next;
  pthread_mutex_unlock(&global_uma_list->mutex);

  uma = node->ptr;
  free_node(node);

  if (uma->rwlockp == NULL) {
    uma->rwlockp = (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t));
    if (__glibc_unlikely(uma->rwlockp == NULL)) {
      handle_error("malloc");
    }
  }
  uma->id = get_uma_id();
  return uma;
}

void free_uma(uma_t *uma) {
  list_node_t *node;

  node = alloc_list_node();
  node->ptr = (void *)uma;

  pthread_mutex_lock(&global_uma_list->mutex);
  node->next = global_uma_list->head;
  global_uma_list->head = node;
  pthread_mutex_unlock(&global_uma_list->mutex);
}

static void *alloc_log_data(log_size_t log_size) {
  list_node_t *node;
  void *data;
  unsigned long count;

  pthread_mutex_lock(&global_data_list[log_size]->mutex);

  count = global_data_list[log_size]->count;

  if (__glibc_unlikely(count == 0)) {
    handle_error("global_data_list does not have anything");
  }

  node = global_data_list[log_size]->head;
  global_data_list[log_size]->head = node->next;
  node->next = NULL;
  global_data_list[log_size]->count -= 1;

  pthread_mutex_unlock(&global_data_list[log_size]->mutex);

  if (__glibc_unlikely(node->ptr == NULL)) {
    handle_error("node->ptr == NULL");
  }


  data = node->ptr;
  free_node(node);

  return data;
}

log_table_t *alloc_log_table(log_table_t *parent, int index,
                             table_type_t type) {
  log_table_t *table;
  list_node_t *node;

  if (local_tables_list == NULL) {
    alloc_list(&local_tables_list);
  }

  if (local_tables_list->count == 0) {
    fill_local_tables_list();
  }

  node = local_tables_list->head;
  local_tables_list->head = node->next;
  local_tables_list->count -= 1;

  if (__glibc_unlikely(node->ptr == NULL)) {
    handle_error("node->ptr == NULL");
  }

  table = node->ptr;
  free_node(node);

  table->count = 0;
  table->type = type;
  table->parent = parent;
  table->index = index;
  table->log_size = LOG_4K;

  return table;
}

log_entry_t *alloc_log_entry(uma_t *uma, log_size_t log_size) {
  log_entry_t *entry;
  list_node_t *node;
  int s;

  if (local_entries_list == NULL) {
    alloc_list(&local_entries_list);
  }

  if (local_entries_list->count == 0) {
    fill_local_entries_list();
  }

  node = local_entries_list->head;
  local_entries_list->head = node->next;
  local_entries_list->count -= 1;

  if (__glibc_unlikely(node->ptr == NULL)) {
    handle_error("node->ptr == NULL");
  }

  entry = node->ptr;
  free_node(node);

  entry->epoch = uma->epoch;
  entry->offset = 0;
  entry->len = 0;
  entry->policy = uma->policy;
  entry->dst = NULL;
  entry->data = alloc_log_data(log_size);

  s = pthread_rwlock_init(entry->rwlockp, NULL);
  if (__glibc_unlikely(s != 0)) {
    handle_error("pthread_rwlock_init");
  }

  return entry;
}

void free_log_entry(log_entry_t *entry, log_size_t log_size, bool sync) {
  int s;
  entry->united = 0;
  entry->data = NULL;
  entry->dst = NULL;

  if (sync) {
    pmem_persist(entry, sizeof(log_entry_t));
  }

  s = pthread_rwlock_destroy(entry->rwlockp);
  if (__glibc_unlikely(s != 0)) {
    handle_error("pthread_rwlock_destroy");
  }

  put_log_local(entry, log_size);
}

void release_local_list(void) {
  list_node_t *node;
  unsigned long nrnodes, i;

  if (local_entries_list == NULL) {
    alloc_list(&local_entries_list);
  }

  nrnodes = local_entries_list->count;

  if (nrnodes == 0)
    return;

  node = local_entries_list->head;

  for (i = 0; i < nrnodes - 1; i++) {
    node = node->next;
  }

  if (__glibc_unlikely(node->next != NULL)) {
    handle_error("data_node->next is not NULL");
  }

  pthread_mutex_lock(&global_entries_list->mutex);

  node->next = global_entries_list->head;
  global_entries_list->head = local_entries_list->head;
  local_entries_list->head = NULL;
  global_entries_list->count += nrnodes;
  local_entries_list->count -= nrnodes;

  if (__glibc_unlikely(local_entries_list->count != 0)) {
    handle_error("local_entries_list->nrnodes != 0");
  }

  pthread_mutex_unlock(&global_entries_list->mutex);
}

void init_env(void) {
  char dirpath[40];
  size_t len;
	int s;

  pmem_path = getenv("PMEM_PATH");
  if (__glibc_unlikely(pmem_path == NULL)) {
    handle_error("PMEM_PATH is NULL.");
  }

  len = strlen(pmem_path);

  if (pmem_path[len - 1] == '/') {
    pmem_path[len - 1] = '\0';
  }

  libnvmmio_pid = getpid();
	
	sprintf(dirpath, DIR_PATH, pmem_path, libnvmmio_pid);
	s = mkdir(dirpath, 0777);
	if (__glibc_unlikely(s != 0)) {
		handle_error("mkdir");
	}
}

void init_global_freelist(void) {
  create_global_tables_list(MAX_FREE_NODES * 10);
  create_global_entries_list(LOG_FILE_SIZE * 32);
  create_global_data_list(LOG_FILE_SIZE * 2);
  create_global_umas_list();
}

void cleanup_logs(void) {
	char log_dir[1024];
	sprintf(log_dir, DIR_PATH, pmem_path, libnvmmio_pid);
	rmlogs(log_dir);
	LIBNVMMIO_DEBUG("removed logs");
}
