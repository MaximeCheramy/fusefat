#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fat.h"

struct fat_options
{
  char* device;
} options;

static struct fuse_opt fat_fuse_opts[] =
{
  { "-device=%s", offsetof(struct fat_options, device), 0 },
};

FILE* debug;

static fat_info_t fat_info;

char * decode_long_file_name(char * name, lfn_entry_t * long_file_name) {

  name[0] = long_file_name->filename1[0];
  name[1] = long_file_name->filename1[2];
  name[2] = long_file_name->filename1[4];
  name[3] = long_file_name->filename1[6];
  name[4] = long_file_name->filename1[8];

  name[5] = long_file_name->filename2[0];
  name[6] = long_file_name->filename2[2];
  name[7] = long_file_name->filename2[4];
  name[8] = long_file_name->filename2[6];
  name[9] = long_file_name->filename2[8];
  name[10] = long_file_name->filename2[10];

  name[11] = long_file_name->filename3[0];
  name[12] = long_file_name->filename3[2];

  name[13] = '\0';

  return name;
}

static time_t convert_datetime_fat_to_time_t(uint16_t date, uint16_t time) {
  int day = date & 0x1f;
  int month = (date & 0x1e0) >> 5;
  int year = 1980 + ((date & 0xfe00) >> 9);

  int seconds = (time & 0x1f) * 2;
  int minutes = (time & 0x7e0) >> 5;
  int hours = (time & 0xf800) >> 11;

  struct tm t = {
    .tm_sec = seconds,
    .tm_min = minutes,
    .tm_hour = hours,
    .tm_mday = day,
    .tm_mon = month - 1,
    .tm_year = year - 1900,
  };

  return mktime(&t);
}

static void read_fat() {
  uint8_t buffer[fat_info.BS.bytes_per_sector * fat_info.BS.table_size_16];
  
  uint32_t i;
  int p = 0;
  uint32_t tmp = 0;

  int fd = open(options.device, O_RDONLY);
  pread(fd, buffer, sizeof(buffer), fat_info.addr_fat[0]);
  close(fd);

  // decodage FAT12
  for (i = 0; i < fat_info.total_clusters; i += 2) {

    // on interprete les 3 octets comme un little endian (24bits) number
    tmp = buffer[p] + ((buffer[p + 1] << 8) & 0xFF00) + ((buffer[p + 2]
        << 16) & 0xFF0000);

    // on extrait les 2 clusters de 12bits
    fat_info.file_alloc_table[i] = (tmp & 0xFFF); // 12 least significant bits
    fat_info.file_alloc_table[i + 1] = (((tmp & 0xFFF000) >> 12) & 0xFFF); // 12 most significant bits

    p += 3;
  }
}

static void mount_fat() {
  fprintf(stderr, "Mount FAT.\n");
  int fd = open(options.device, O_RDONLY);
  if (fd > 0) {
    pread(fd, &fat_info.BS, sizeof(fat_info.BS), 0);
    close(fd);
 
    fprintf(stderr, "%d bytes per logical sector\n", fat_info.BS.bytes_per_sector);
    fprintf(stderr, "%d bytes per clusters\n", fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster);
 
    fat_info.addr_fat = (unsigned int*) malloc(sizeof(unsigned int) * fat_info.BS.table_count);
    int i;
    for (i = 0; i < fat_info.BS.table_count; i++) {
      fat_info.addr_fat[i] = (fat_info.BS.reserved_sector_count + i * fat_info.BS.table_size_16) * fat_info.BS.bytes_per_sector;
    }
    fat_info.addr_root_dir = (fat_info.BS.reserved_sector_count + fat_info.BS.table_count * fat_info.BS.table_size_16) * fat_info.BS.bytes_per_sector;
    fat_info.addr_data = fat_info.addr_root_dir + (fat_info.BS.root_entry_count * 32);
    if (fat_info.BS.total_sectors_16 > 0)
      fat_info.total_clusters = fat_info.BS.total_sectors_16 / fat_info.BS.sectors_per_cluster;
    else
      fat_info.total_clusters = fat_info.BS.total_sectors_32 / fat_info.BS.sectors_per_cluster;

    fprintf(stderr, "First FAT starts at byte %u (sector %u)\n", fat_info.addr_fat[0], fat_info.addr_fat[0] / fat_info.BS.bytes_per_sector);
    fprintf(stderr, "Root directory starts at byte %u (sector %u)\n", fat_info.addr_root_dir, fat_info.addr_root_dir / fat_info.BS.bytes_per_sector);
    fprintf(stderr, "Data area starts at byte %u (sector %u)\n", fat_info.addr_data, fat_info.addr_data / fat_info.BS.bytes_per_sector);
    fprintf(stderr, "Total clusters : %d\n", fat_info.total_clusters);

    fat_info.file_alloc_table = (unsigned int*) malloc(sizeof(unsigned int) * fat_info.BS.table_size_16 * fat_info.BS.bytes_per_sector);

    read_fat();
  }
}

static void fat_dir_entry_to_directory_entry(char *filename, fat_dir_entry_t *dir, directory_entry_t *entry) {
  strcpy(entry->name, filename);
  entry->cluster = dir->cluster_pointer;
  entry->attributes = dir->file_attributes;
  entry->size = dir->file_size;
  entry->access_time = 
      convert_datetime_fat_to_time_t(dir->last_access_date, 0);
  entry->modification_time =
      convert_datetime_fat_to_time_t(dir->last_modif_date, dir->last_modif_time);
  entry->creation_time = 
      convert_datetime_fat_to_time_t(dir->create_date, dir->create_time);
}

static void read_data(void * buf, size_t count, off_t offset) {
  int fd = open(options.device, O_RDONLY);
  pread(fd, buf, count, offset);
  close(fd);
}

static directory_t * open_root_dir() {
  directory_t *dir = malloc(sizeof(directory_t));
  fat_dir_entry_t *root_dir = malloc(sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count);
  dir->total_entries = 0;

  read_data(root_dir, sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count, fat_info.addr_root_dir);

  int i;
  for (i = 0; i < fat_info.BS.root_entry_count; i++) {
    char filename[256];
    uint8_t i_filename = 0;
    if (root_dir[i].utf8_short_name[0] != 0xE5 &&
        root_dir[i].file_attributes == 0x0F) {
      if (((lfn_entry_t*) &root_dir[i])->seq_number & 0x40) {
        int j;
        uint8_t seq = ((lfn_entry_t*) &root_dir[i])->seq_number - 0x40;
        i += seq;
        for (j = 1; j <= seq; j++) {
          decode_long_file_name(filename + i_filename,
              ((lfn_entry_t*) &root_dir[i - j]));
          i_filename += 13;
        }
        fat_dir_entry_to_directory_entry(filename, &root_dir[i], &(dir->entries[dir->total_entries]));
        dir->total_entries++;
      } 
    }
  }

  free(root_dir);

  return dir;
}

static int open_next_dir(directory_t * prev_dir, directory_t * next_dir, char * name) {
  fprintf(debug, "open_next_dir, name = %s\n", name);
  fflush(debug);

  fat_dir_entry_t sub_dir[16]; // XXX
  int next = 0;
  int i;

  for (i = 0; i < (prev_dir->total_entries); i++) {
    if (strcmp(prev_dir->entries[i].name, name) == 0) {
      if ((prev_dir->entries[i].attributes & 0x10) == 0x10) { //c'est bien un repe
        next = prev_dir->entries[i].cluster;
        break;
      } else {
        return 2;
      }
    }
  }

  if (next == 0) {
    return 1;
  }

  read_data(sub_dir, sizeof(sub_dir), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);

  next_dir->total_entries = 0;
  for (i = 0; i < 16; i++) {
    char filename[256];
    uint8_t i_filename = 0;
    if (sub_dir[i].utf8_short_name[0] != 0xE5 &&
        sub_dir[i].file_attributes == 0x0F) { // TODO: constantes pour les flags.
      if (((lfn_entry_t*) &sub_dir[i])->seq_number & 0x40) {
        int j;
        uint8_t seq = ((lfn_entry_t*) &sub_dir[i])->seq_number
            - 0x40;
        i += seq;
        for (j = 1; j <= seq; j++) {
          decode_long_file_name(filename + i_filename,
              ((lfn_entry_t*) &sub_dir[i - j]));
          i_filename += 13;
        }

        fat_dir_entry_to_directory_entry(filename, &sub_dir[i], &(next_dir->entries[next_dir->total_entries]));
        next_dir->total_entries++;
      }
    }
  }
  return 0;
}

static void split_dir_filename(const char * path, char * dir, char * filename) {
  char *p = strrchr(path, '/');
  strcpy(filename, p+1);
  for (; path < p; path++, dir++) {
    *dir = *path;
  } 
  *dir = '\0';
}

static directory_t * open_dir_from_path(const char *path) {
  fprintf(debug, "open_dir_from_path %s\n", path);
  fflush(debug);

  if (path[0] == '\0' || strcmp(path, "/") == 0)
    return open_root_dir();

  // Only absolute paths.
  if (path[0] != '/')
    return NULL;

  char buf[256];
  int i = 1;
  while (path[i] == '/')
    i++;

  directory_t * dir = open_root_dir();

  int j = 0;
  do {
    if (path[i] == '/' || path[i] == '\0') {
      while (path[i] == '/')
        i++;
      buf[j] = '\0';
      j = 0;

      if (open_next_dir(dir, dir, buf) != 0)
        return NULL;
    } else {
      buf[j] = path[i];
    }
    j++;
  } while (path[i++] != '\0');

  return dir;
}


static int fat_getattr(const char *path, struct stat *stbuf)
{
  fprintf(debug, "fat_getattr %s\n", path);
  fflush(debug);

  int res = 0;

  memset(stbuf, 0, sizeof(struct stat));
  stbuf->st_nlink = 2; // XXX
  stbuf->st_mode = 0755;

  if(strcmp(path, "/") == 0) {
    stbuf->st_mode |= S_IFDIR;
  } else {
    directory_t *dir;
    char filename[256];
    char * pathdir = malloc(strlen(path) + 1);
    split_dir_filename(path, pathdir, filename);
    if ((dir = open_dir_from_path(pathdir)) == NULL)
      return -ENOENT;
    free(pathdir);

    int i;
    for (i = 0; i < dir->total_entries; i++) {
      if (strcmp(dir->entries[i].name, filename) == 0) {
          break;
      }
    }
    if (i == dir->total_entries) {
      free(dir);
      return -ENOENT;
    } else {
      if (dir->entries[i].attributes & 0x01) { // Read Only
        stbuf->st_mode &= ~0111;
      }
      if (dir->entries[i].attributes & 0x10) { // Dir.
        stbuf->st_mode |= S_IFDIR;
      } else {
        stbuf->st_mode |= S_IFREG;
      }
      stbuf->st_atime = dir->entries[i].access_time;
      stbuf->st_mtime = dir->entries[i].modification_time;
      stbuf->st_ctime = dir->entries[i].creation_time;
    }
    free(dir);
  }

  return res;
}

static int fat_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
  (void) offset;
  (void) fi;
  int i;
  directory_t *dir;

  fprintf(debug, "fat_readdir %s\n", path);
  fflush(debug);

  if ((dir = open_dir_from_path(path)) == NULL)
    return -ENOENT;

  for (i = 0; i < dir->total_entries; i++) {
    filler(buf, dir->entries[i].name, NULL, 0);
  }
  free(dir);

  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);

  return 0;
}

static int fat_open(const char *path, struct fuse_file_info *fi)
{
  directory_t *dir;

  if ((dir = open_dir_from_path(path)) == NULL)
    return -ENOENT;

  if((fi->flags & 3) != O_RDONLY)
    return -EACCES;

  // TODO

  return 0;
}

static int fat_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
  directory_t *dir;
  int count = 0;
  
  if ((dir = open_dir_from_path(path)) == NULL)
    return -ENOENT;

  while (size) {
    
  }
  

  // TODO


/*  int fd = open(options.device, O_RDONLY);
  pread(fd, dir, sizeof(dir), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
  close(fd);*/

  return count;
}

static struct fuse_operations fat_oper = {
    .getattr  = fat_getattr,
    .readdir  = fat_readdir,
    .open = fat_open,
    .read = fat_read,
};

int main(int argc, char *argv[])
{
  int ret;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
        
  if (fuse_opt_parse(&args, &options, fat_fuse_opts, NULL) == -1)
    return -1; /** error parsing **/

  fprintf(stderr, "device : %s\n", options.device);

  debug = fopen("/tmp/debugfuse", "w+");
  mount_fat();
  
  ret = fuse_main(args.argc, args.argv, &fat_oper);
  fuse_opt_free_args(&args);

  return ret;
}
