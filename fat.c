#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

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

static void write_data(void * buf, size_t count, off_t offset) {
  int fd = open(options.device, O_WRONLY);
  pwrite(fd, buf, count, offset);
  close(fd);
}

static void read_data(void * buf, size_t count, off_t offset) {
  int fd = open(options.device, O_RDONLY);
  pread(fd, buf, count, offset);
  close(fd);
}

static char * lfn_to_sfn(char * filename) {
  char * lfn = strdup(filename);
  char * sfn = malloc(12);

  // To upper case.
  int i = 0;
  while (lfn[i] != '\0') {
    lfn[i] = toupper(lfn[i]);
    i++;
  }

  // TODO: Convert to OEM (=> '_').

  // Strip all leading and embedded spaces
  int j = 0;
  i = 0;
  while (lfn[i] != '\0') {
    if (lfn[i] != ' ') {
      lfn[j] = lfn[i];
      j++;
    }
    i++;
  }
  lfn[j] = '\0';

  char * ext = strrchr(lfn, '.');
  int has_ext = (ext != NULL) && (lfn + j - ext - 1 <= 3);

  if (has_ext) {
    // Copy first 8 caracters.
    i = 0;
    j = 0;
    while (&lfn[i] <= ext) {
      if (lfn[i] != '.' && j < 8) {
        sfn[j] = lfn[i];
        j++;
      }
      i++;
    }
  
    // padding
    while (j < 8)
      sfn[j++] = ' ';

    // Copy extension.
    sfn[j++] = '.';
    while (lfn[i] != '\0') {
      sfn[j++] = lfn[i];
      i++;
    }

    while (j < 12)
      sfn[j++] = ' ';
  } else {
    i = 0;
    j = 0;
    while (lfn[i] != '\0' && j < 8) {
      if (lfn[i] != '.') {
        sfn[j] = lfn[i];
        j++;
      }
      i++;
    }
    while (j < 12)
      sfn[j++] = ' ';
  }

  // TODO: numeric-tail generation.

	free(lfn);
  return sfn;
}

static char * decode_long_file_name(char * name, lfn_entry_t * long_file_name) {

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

static void encode_long_file_name(char * name, lfn_entry_t * long_file_name, int n_entries) {
 // TODO: Checksum.
  long_file_name[0].seq_number = 0x40 + n_entries;
  int i, j;
  int last = 0;
  for (i = n_entries - 1; i >= 0; i--) {
    long_file_name[i].attributes = 0x0f;
    if (i != n_entries - 1)
      long_file_name[i].seq_number = i + 1;
    for (j = 0; j < 5; j++) {
      if (last) {
        long_file_name[i].filename1[j * 2] = 0xFF;
        long_file_name[i].filename1[j * 2 + 1] = 0xFF;
      } else if (name[j] != '\0') {
        long_file_name[i].filename1[j * 2] = name[i * 13 + j];
        long_file_name[i].filename1[j * 2 + 1] = 0;
      } else {
        long_file_name[i].filename1[j * 2] = 0;
        long_file_name[i].filename1[j * 2 + 1] = 0;
        last = 1;
      } 
    }
    for (j = 5; j < 11; j++) {
      if (last) {
        long_file_name[i].filename2[(j - 5) * 2] = 0xFF;
        long_file_name[i].filename2[(j - 5) * 2 + 1] = 0xFF;
      } else if (name[j] != '\0') {
        long_file_name[i].filename2[(j - 5) * 2] = name[i * 13 + j];
        long_file_name[i].filename2[(j - 5) * 2 + 1] = 0;
      } else {
        long_file_name[i].filename2[(j - 5) * 2] = 0;
        long_file_name[i].filename2[(j - 5) * 2 + 1] = 0;
        last = 1;
      }
    }

    if (last) {
      long_file_name[i].filename3[0] = 0xFF;
      long_file_name[i].filename3[1] = 0xFF;
    } else if (name[11] != '\0') {
      long_file_name[i].filename3[0] = name[i * 13 + 11];
      long_file_name[i].filename3[1] = 0;
    } else {
      long_file_name[i].filename3[0] = 0;
      long_file_name[i].filename3[1] = 0;
      last = 1;
    }

    if (last) {
      long_file_name[i].filename3[0] = 0xFF;
      long_file_name[i].filename3[1] = 0xFF;
    } else if (name[12] != '\0') {
      long_file_name[i].filename3[0] = name[i * 13 + 12];
      long_file_name[i].filename3[1] = 0;
    } else {
      long_file_name[i].filename3[0] = 0;
      long_file_name[i].filename3[1] = 0;
      last = 1;
    }
  }
}

static int last_cluster() {
  if (fat_info.fat_type == FAT12) {
    return 0xFFF;
  } else if (fat_info.fat_type == FAT16) {
    return 0xFFFF;
  } else {
    return 0x0FFFFFFF;
  }
}

static int is_free_cluster(int cluster) {
  return cluster == 0;
}

static int is_last_cluster(int cluster) {
  if (fat_info.fat_type == FAT12) {
    return cluster >= 0xFF8 && cluster <= 0xFFF;
  } else if (fat_info.fat_type == FAT16) {
    return cluster >= 0xFFF8 && cluster <= 0xFFFF;
  } else {
    return cluster >= 0x0FFFFFF8 && cluster <= 0x0FFFFFFF;
  }
}

static int is_used_cluster(int cluster) {
  if (fat_info.fat_type == FAT12) {
    return cluster >= 0x002 && cluster <= 0xFEF;
  } else if (fat_info.fat_type == FAT16) {
    return cluster >= 0x0002 && cluster <= 0xFFEF;
  } else {
    return cluster >= 0x00000002 && cluster <= 0x0FFFFFEF;
  }
}

static void convert_time_t_to_datetime_fat(time_t time, fat_time_t *timefat, fat_date_t *datefat) {
  #define MINUTES 60
  #define HOURS 3600
  #define DAYS 86400

  int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days_per_month_leap[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  int year = 1970;
  int month = 0;
  int day = 0;
  int hours = 0;
  int min = 0;
  int sec = 0;

  int secs_year = DAYS * 365;
  int *dpm = days_per_month;

  while (time) {
    if (time >= secs_year) {
      year++;
      time -= secs_year;

      if (!(year % 400 && (year % 100 == 0 || (year & 3)))) {
        secs_year = DAYS * 366;
        dpm = days_per_month_leap;
      } else {
        secs_year = DAYS * 365;
        dpm = days_per_month;
      }
    } else {
      if (time >= dpm[month] * DAYS) {
        time -= dpm[month] * DAYS;
        month++;
      } else {
        day = time / DAYS;
        time -= day * DAYS;
        hours = time / HOURS;
        time -= hours * HOURS;
        min = time / MINUTES;
        time -= min * MINUTES;
        sec = time;
        time = 0;
      }
    }
  }

  datefat->year = year - 1980;
  datefat->month = month + 1;
  datefat->day = day + 1;
  if (timefat) {
    timefat->hours = hours;
    timefat->minutes = min;
    timefat->seconds2 = sec / 2;
  }
}

static time_t convert_datetime_fat_to_time_t(fat_date_t *date, fat_time_t *time) {
  int seconds, minutes, hours;

  if (time) {
    seconds = time->seconds2 * 2;
    minutes = time->minutes;
    hours = time->hours;
  } else {
    seconds = minutes = hours = 0;
  }

  struct tm t = {
    .tm_sec = seconds,
    .tm_min = minutes,
    .tm_hour = hours,
    .tm_mday = date->day,
    .tm_mon = date->month - 1,
    .tm_year = date->year + 80,
  };

  return mktime(&t);
}

static void read_fat() {
  uint8_t buffer[fat_info.BS.bytes_per_sector * fat_info.table_size];
  
  uint32_t i;
  int p = 0;
  uint32_t tmp = 0;

  read_data(buffer, sizeof(buffer), fat_info.addr_fat[0]);

  if (fat_info.fat_type == FAT12) {
    // decodage FAT12
    for (i = 0; i < fat_info.total_data_clusters; i += 2) {
      tmp = buffer[p] + (buffer[p + 1] << 8) + (buffer[p + 2] << 16);

      // on extrait les 2 clusters de 12bits
      fat_info.file_alloc_table[i] = (tmp & 0xFFF); // 12 least significant bits
      fat_info.file_alloc_table[i + 1] = (tmp >> 12); // 12 most significant bits

      p += 3;
    }
  } else if (fat_info.fat_type == FAT16) {
    for (i = 0; i < fat_info.total_data_clusters; i++) {
      fat_info.file_alloc_table[i] = buffer[i * 2] + (buffer[i * 2 + 1] << 8);
    }
  } else if (fat_info.fat_type == FAT32) {
    for (i = 0; i < fat_info.total_data_clusters; i++) {
      fat_info.file_alloc_table[i] = buffer[i * 4] + (buffer[i * 4 + 1] << 8) + (buffer[i * 4 + 2] << 16) + (buffer[i * 4 + 3] << 24);
    }
  }
}

static void write_fat() {
  uint8_t buffer[fat_info.BS.bytes_per_sector * fat_info.table_size];
  
  uint32_t i;
  int p = 0;
  uint32_t tmp = 0;

  if (fat_info.fat_type == FAT12) {
    for (i = 0; i < fat_info.total_data_clusters; i += 2) {
      tmp = (fat_info.file_alloc_table[i + 1] << 12) + (fat_info.file_alloc_table[i] & 0xFFF);
      buffer[p++] = tmp & 0x0000FF;
      buffer[p++] = tmp & 0x00FF00;
      buffer[p++] = tmp & 0xFF0000;
    }
  } else if (fat_info.fat_type == FAT16) {
    for (i = 0; i < fat_info.total_data_clusters; i++) {
      buffer[i*2] = fat_info.file_alloc_table[i] & 0x00FF;
      buffer[i*2 + 1] = fat_info.file_alloc_table[i] & 0xFF00;
    }
  } else if (fat_info.fat_type == FAT32) {
    for (i = 0; i < fat_info.total_data_clusters; i++) {
      buffer[i*4]     = fat_info.file_alloc_table[i] & 0x000000FF;
      buffer[i*4 + 1] = fat_info.file_alloc_table[i] & 0x0000FF00;
      buffer[i*4 + 2] = fat_info.file_alloc_table[i] & 0x00FF0000;
      buffer[i*4 + 3] = fat_info.file_alloc_table[i] & 0xFF000000;
    }
  }
 
  for (i = 0; i < fat_info.BS.table_count; i++) {
    write_data(buffer, sizeof(buffer), fat_info.addr_fat[i]);
  }
}

static void write_fat_entry(int index) {
	int i;
	if (fat_info.fat_type == FAT12) {
		uint32_t tmp;
		char buffer[3]; // 24 bits : 2 entries
		if (index % 2 == 0) {
      tmp = (fat_info.file_alloc_table[index + 1] << 12) + (fat_info.file_alloc_table[index] & 0xFFF);
		} else {
      tmp = (fat_info.file_alloc_table[index] << 12) + (fat_info.file_alloc_table[index - 1] & 0xFFF);
		}
    buffer[0] = tmp & 0x0000FF;
    buffer[1] = tmp & 0x00FF00;
    buffer[2] = tmp & 0xFF0000;

		for (i = 0; i < fat_info.BS.table_count; i++) {
			write_data(buffer, sizeof(buffer), fat_info.addr_fat[i] + index * 12);
		}
  } else if (fat_info.fat_type == FAT16) {
		char buffer[2];
    buffer[0] = fat_info.file_alloc_table[index] & 0x00FF;
    buffer[1] = fat_info.file_alloc_table[index] & 0xFF00;
		for (i = 0; i < fat_info.BS.table_count; i++) {
			write_data(buffer, sizeof(buffer), fat_info.addr_fat[i] + index * 16);
		}
  } else if (fat_info.fat_type == FAT32) {
		char buffer[4];
    buffer[0] = fat_info.file_alloc_table[index] & 0x000000FF;
    buffer[1] = fat_info.file_alloc_table[index] & 0x0000FF00;
    buffer[2] = fat_info.file_alloc_table[index] & 0x00FF0000;
    buffer[3] = fat_info.file_alloc_table[index] & 0xFF000000;
		for (i = 0; i < fat_info.BS.table_count; i++) {
			write_data(buffer, sizeof(buffer), fat_info.addr_fat[i] + index * 32);
		}
  }
 
}

static void mount_fat() {
  fprintf(stderr, "Mount FAT.\n");
  int fd = open(options.device, O_RDONLY);
  if (fd > 0) {
		pread(fd, &fat_info.BS, sizeof(fat_BS_t), 0);
    
    if (fat_info.BS.table_size_16 == 0) { // Si 0 alors on considère qu'on est en FAT32.
      fat_info.ext_BIOS_16 = NULL;
      fat_info.ext_BIOS_32 = malloc(sizeof(fat_extended_BIOS_32_t));
      pread(fd, fat_info.ext_BIOS_32, sizeof(fat_extended_BIOS_32_t), sizeof(fat_BS_t));
      fat_info.table_size = fat_info.ext_BIOS_32->table_size_32;
    } else {
      fat_info.ext_BIOS_32 = NULL;
      fat_info.ext_BIOS_16 = malloc(sizeof(fat_extended_BIOS_16_t));
      pread(fd, fat_info.ext_BIOS_16, sizeof(fat_extended_BIOS_16_t), sizeof(fat_BS_t));
      fat_info.table_size = fat_info.BS.table_size_16;
    }
    close(fd);

    fprintf(stderr, "table size : %d\n", fat_info.table_size);


    fprintf(stderr, "%d bytes per logical sector\n", fat_info.BS.bytes_per_sector);
    fprintf(stderr, "%d bytes per clusters\n", fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster);
 
    fat_info.addr_fat = (unsigned int*) malloc(sizeof(unsigned int) * fat_info.BS.table_count);
    
    int i;
    for (i = 0; i < fat_info.BS.table_count; i++) {
      fat_info.addr_fat[i] = (fat_info.BS.reserved_sector_count + i * fat_info.table_size) * fat_info.BS.bytes_per_sector;
    }
    fat_info.addr_root_dir = (fat_info.BS.reserved_sector_count + fat_info.BS.table_count * fat_info.table_size) * fat_info.BS.bytes_per_sector;
    fat_info.addr_data = fat_info.addr_root_dir + (fat_info.BS.root_entry_count * sizeof(fat_dir_entry_t));
    if (fat_info.BS.total_sectors_16 > 0)
      fat_info.total_data_clusters = fat_info.BS.total_sectors_16 / fat_info.BS.sectors_per_cluster - fat_info.addr_data / (fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster);
    else
      fat_info.total_data_clusters = fat_info.BS.total_sectors_32 / fat_info.BS.sectors_per_cluster - fat_info.addr_data / (fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster);

    if (fat_info.total_data_clusters < 4086) {
      fat_info.fat_type = FAT12;
      fprintf(stderr, "FAT Type : FAT12\n");
    } else if (fat_info.total_data_clusters < 65526) {
      fat_info.fat_type = FAT16;
      fprintf(stderr, "FAT Type : FAT16\n");
    } else {
      fat_info.fat_type = FAT32;
      fprintf(stderr, "FAT Type : FAT32\n");
    }

    fprintf(stderr, "First FAT starts at byte %u (sector %u)\n", fat_info.addr_fat[0], fat_info.addr_fat[0] / fat_info.BS.bytes_per_sector);
    fprintf(stderr, "Root directory starts at byte %u (sector %u)\n", fat_info.addr_root_dir, fat_info.addr_root_dir / fat_info.BS.bytes_per_sector);
    fprintf(stderr, "Data area starts at byte %u (sector %u)\n", fat_info.addr_data, fat_info.addr_data / fat_info.BS.bytes_per_sector);
    fprintf(stderr, "Total clusters : %d\n", fat_info.total_data_clusters);

    fat_info.file_alloc_table = (unsigned int*) malloc(sizeof(unsigned int) * fat_info.total_data_clusters);

    read_fat();
  }
}

static void fat_dir_entry_to_directory_entry(char *filename, fat_dir_entry_t *dir, directory_entry_t *entry) {
  strcpy(entry->name, filename);
  entry->cluster = dir->cluster_pointer;
  entry->attributes = dir->file_attributes;
  entry->size = dir->file_size;
  entry->access_time = 
      convert_datetime_fat_to_time_t(&dir->last_access_date, NULL);
  entry->modification_time =
      convert_datetime_fat_to_time_t(&dir->last_modif_date, &dir->last_modif_time);
  entry->creation_time = 
      convert_datetime_fat_to_time_t(&dir->create_date, &dir->create_time);
}

static directory_entry_t * decode_lfn_entry(lfn_entry_t* fdir) {
  int j;
  char filename[256];
  uint8_t i_filename = 0;
  uint8_t seq = fdir->seq_number - 0x40;
  for (j = seq-1; j >= 0; j--) {
    decode_long_file_name(filename + i_filename, &fdir[j]);
    i_filename += 13;
  }
  directory_entry_t *dir_entry = malloc(sizeof(directory_entry_t));
  fat_dir_entry_to_directory_entry(filename, (fat_dir_entry_t*)&fdir[seq], dir_entry);
  return dir_entry;
}

static void decode_short_file_name(char *filename, fat_dir_entry_t *fdir) {
  int j, k;
	int notspace = 0;

  // Copy basis name.
  for (j = 7; j >= 0; j--) {
		if (notspace || fdir->utf8_short_name[j] != ' ') {
			if (!notspace) {
				notspace = j;
			}
			if (fdir->reserved && 0x08) {
				filename[j] = tolower(fdir->utf8_short_name[j]);
			} else {
				filename[j] = fdir->utf8_short_name[j];
			}
		}
  }
  
	notspace++; // notspace est la position du premier caractère != ' '
  filename[notspace++] = '.';

	int notspaceext = -2;
  // Copy extension.
  for (k = 2; k >= 0; k--) {
		if (notspaceext > 0 || fdir->file_extension[k] != ' ') {
			if (notspaceext <= 0) {
				notspaceext = k;
			}
			if (fdir->reserved && 0x10) {
				filename[notspace + k] = tolower(fdir->file_extension[k]);
			} else {
				filename[notspace + k] = fdir->file_extension[k];
			}
		}
  }

	filename[notspace + notspaceext + 1] = '\0';


}

static directory_entry_t * decode_sfn_entry(fat_dir_entry_t *fdir) {
  char filename[256];
	decode_short_file_name(filename, fdir);
  directory_entry_t *dir_entry = malloc(sizeof(directory_entry_t));
  fat_dir_entry_to_directory_entry(filename, fdir, dir_entry);
  return dir_entry;
}

static int alloc_cluster(int n) {
  if (n <= 0) {
    return last_cluster();
  }
  int next = alloc_cluster(n - 1);
  int i;
  for (i = 0; i < fat_info.total_data_clusters; i++) {
    if (fat_info.file_alloc_table[i] == 0) {
      fat_info.file_alloc_table[i] = next;
			write_fat_entry(i);
      return i;
    }
  }
  return -1;
}


static int updatedate_dir_entry(int cluster, char * filename, time_t accessdate, time_t modifdate) {
  directory_entry_t *dir_entry;
  int n_clusters = 0;

  if (cluster > 0) {
    int next = cluster;
    while (!is_last_cluster(next)) {
      next = fat_info.file_alloc_table[next];
      n_clusters++;
    }
  
    int n_dir_entries = fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster / sizeof(fat_dir_entry_t);
    fat_dir_entry_t * fdir = malloc(n_dir_entries * sizeof(fat_dir_entry_t) * n_clusters);
  
    int c = 0;
    next = cluster;
    while (!is_last_cluster(next)) {
      read_data(fdir + c * n_dir_entries, n_dir_entries * sizeof(fat_dir_entry_t), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
      next = fat_info.file_alloc_table[next];
      c++;
    }
  
    int i;
    for (i = 0; i < n_dir_entries * n_clusters && fdir[i].utf8_short_name[0]; i++) {
      if ((unsigned char)fdir[i].utf8_short_name[0] != 0xE5) {
        if (fdir[i].file_attributes == 0x0F && ((lfn_entry_t*) &fdir[i])->seq_number & 0x40) {
          dir_entry = decode_lfn_entry((lfn_entry_t*) &fdir[i]);
          uint8_t seq = ((lfn_entry_t*) &fdir[i])->seq_number - 0x40;
          i += seq;
        } else {
          dir_entry = decode_sfn_entry(&fdir[i]);
        }
  
        if (strcmp(filename, dir_entry->name) == 0) {
          next = cluster;
          while (i >= n_dir_entries) {
            i -= n_dir_entries;
            next = fat_info.file_alloc_table[next];
          }
          convert_time_t_to_datetime_fat(accessdate, NULL, &(fdir[i].last_access_date));
          convert_time_t_to_datetime_fat(modifdate, &(fdir[i].last_modif_time), &(fdir[i].last_modif_date));
          write_data(&fdir[i], sizeof(fat_dir_entry_t), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector + i * sizeof(fat_dir_entry_t));
					free(fdir);
          return 0;
        }
        free(dir_entry);
      }
    }
  } else {
    fat_dir_entry_t *fdir = malloc(sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count);
    read_data(fdir, sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count, fat_info.addr_root_dir);
 
    int i;
    for (i = 0; i < fat_info.BS.root_entry_count && fdir[i].utf8_short_name[0]; i++) {
      if ((unsigned char)fdir[i].utf8_short_name[0] != 0xE5) {
        if (fdir[i].file_attributes == 0x0F && ((lfn_entry_t*) &fdir[i])->seq_number & 0x40) {
          dir_entry = decode_lfn_entry((lfn_entry_t*) &fdir[i]);
          uint8_t seq = ((lfn_entry_t*) &fdir[i])->seq_number - 0x40;
          i += seq;
        } else {
          dir_entry = decode_sfn_entry(&fdir[i]);
        }
  
        if (strcmp(filename, dir_entry->name) == 0) {
          convert_time_t_to_datetime_fat(accessdate, NULL, &(fdir[i].last_access_date));
          convert_time_t_to_datetime_fat(modifdate, &(fdir[i].last_modif_time), &(fdir[i].last_modif_date));
          write_data(&fdir[i], sizeof(fat_dir_entry_t), fat_info.addr_root_dir + i * sizeof(fat_dir_entry_t));
					free(fdir);
  
          return 0;
        }
        free(dir_entry);
      }
    }
  }


  return 1;
}

static int delete_dir_entry(fat_dir_entry_t *fdir, const char *name, int n) {
	fprintf(debug, "delete_dir_entry %s %d\n", name, n);
	fflush(debug);
  char filename[256];
  int i;

  for (i = 0; i < n && fdir[i].utf8_short_name[0]; i++) {
		fprintf(debug, "> %s\n", fdir[i].utf8_short_name);
    if ((unsigned char)fdir[i].utf8_short_name[0] != 0xE5) {
      if (fdir[i].file_attributes == 0x0F && ((lfn_entry_t*) &fdir[i])->seq_number & 0x40) {

			  int j;
			  uint8_t i_filename = 0;
			  uint8_t seq = ((lfn_entry_t*) &fdir[i])->seq_number - 0x40;
			  for (j = seq-1; j >= 0; j--) {
			    decode_long_file_name(filename + i_filename, (lfn_entry_t*) &fdir[i+j]);
			    i_filename += 13;
			  }

				fprintf(debug, "cmp %s %s\n", filename, name);
				fflush(debug);
				if (strcmp(filename, name) == 0) {
					for (j = seq; j >= 0; j--) {
						fdir[i+j].utf8_short_name[0] = 0xE5;
					}
					return 0;
				}
        i += seq;
      } else {
        decode_short_file_name(filename, &fdir[i]);
				if (strcmp(filename, name) == 0) {
					fdir[i].utf8_short_name[0] = 0xE5;
					return 0;
				}
      }
    }
  }
	return 1;
}

static void read_dir_entries(fat_dir_entry_t *fdir, directory_t *dir, int n) {
	fprintf(debug, "read_dir_entries\n");
	fflush(debug);
  int i;
  for (i = 0; i < n && fdir[i].utf8_short_name[0]; i++) {
    directory_entry_t * dir_entry;

    if ((unsigned char)fdir[i].utf8_short_name[0] != 0xE5) {
      if (fdir[i].file_attributes == 0x0F && ((lfn_entry_t*) &fdir[i])->seq_number & 0x40) {
        dir_entry = decode_lfn_entry((lfn_entry_t*) &fdir[i]);
        uint8_t seq = ((lfn_entry_t*) &fdir[i])->seq_number - 0x40;
        i += seq;
      } else {
        dir_entry = decode_sfn_entry(&fdir[i]);
      }
      dir_entry->next = dir->entries;
      dir->entries = dir_entry;
      dir->total_entries++;
    }
  }
}

static void delete_file_dir(int cluster, const char * name) {
  int n_dir_entries = fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster / sizeof(fat_dir_entry_t);
	if (cluster >= 0) {

	  int n_clusters = 0;
	  int next = cluster;
	  while (!is_last_cluster(next)) {
	    next = fat_info.file_alloc_table[next];
	    n_clusters++;
	  }
	
	  fat_dir_entry_t * sub_dir = malloc(n_dir_entries * sizeof(fat_dir_entry_t) * n_clusters);
	
	  int c = 0;
	  next = cluster;
	  while (!is_last_cluster(next)) {
	    read_data(sub_dir + c * n_dir_entries, n_dir_entries * sizeof(fat_dir_entry_t), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
	    next = fat_info.file_alloc_table[next];
	    c++;
	  }
	
		if (delete_dir_entry(sub_dir, name, n_dir_entries * n_clusters) == 0) {
	
			c = 0;
			next = cluster;
		  while (!is_last_cluster(next)) {
				write_data(sub_dir + c * n_dir_entries, n_dir_entries * sizeof(fat_dir_entry_t), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
		    next = fat_info.file_alloc_table[next];
		    c++;
			}
	
		} else {
			fprintf(debug, "delete_file_dir failed\n");
		}
	
	} else {
    fat_dir_entry_t *root_dir = malloc(sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count);
    read_data(root_dir, sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count, fat_info.addr_root_dir);
		if (delete_dir_entry(root_dir, name, n_dir_entries) == 0) {
			write_data(root_dir, sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count, fat_info.addr_root_dir);
		} else {
			fprintf(debug, "delete_file_dir failed\n");
		}
	}
}

static void open_dir(int cluster, directory_t *dir) {
  int n_clusters = 0;
  int next = cluster;
  while (!is_last_cluster(next)) {
    next = fat_info.file_alloc_table[next];
    n_clusters++;
  }

  int n_dir_entries = fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster / sizeof(fat_dir_entry_t);
  fat_dir_entry_t * sub_dir = malloc(n_dir_entries * sizeof(fat_dir_entry_t) * n_clusters);

  int c = 0;
  next = cluster;
  while (!is_last_cluster(next)) {
    read_data(sub_dir + c * n_dir_entries, n_dir_entries * sizeof(fat_dir_entry_t), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
    next = fat_info.file_alloc_table[next];
    c++;
  }

  dir->cluster = cluster;
  dir->total_entries = 0;
  dir->entries = NULL;

  read_dir_entries(sub_dir, dir, n_dir_entries * n_clusters);
}

static directory_t * open_root_dir() {
	fprintf(debug, "open_root_dir\n");
	fflush(debug);
  directory_t *dir = malloc(sizeof(directory_t));

  if (fat_info.fat_type == FAT32) {
    open_dir(fat_info.ext_BIOS_32->cluster_root_dir, dir);
  } else {
    fat_dir_entry_t *root_dir = malloc(sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count);
  
    read_data(root_dir, sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count, fat_info.addr_root_dir);
  
    dir->cluster = -1;
    dir->total_entries = 0;
    dir->entries = NULL;
  
    read_dir_entries(root_dir, dir, fat_info.BS.root_entry_count);
  
    free(root_dir);
  }

  return dir;
}

static int open_next_dir(directory_t * prev_dir, directory_t * next_dir, char * name) {
  fprintf(debug, "open_next_dir, name = %s\n", name);
	fprintf(debug, "open_next_dir, prev_dir->cluster = %d\n", prev_dir->cluster);
  fflush(debug);

  int next = 0;

  directory_entry_t *dentry = prev_dir->entries;
  while (dentry) {
    if (strcmp(dentry->name, name) == 0) {
      if ((dentry->attributes & 0x10) == 0x10) { //c'est bien un repe
        next = dentry->cluster;
        break;
      } else {
        return 2;
      }
    }
    dentry = dentry->next;
  }

  if (next == 0) {
    return 1;
  }
 
  open_dir(next, next_dir);

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
      buf[j] = '\0';

      if (j > 0 && open_next_dir(dir, dir, buf) != 0)
        return NULL;

      j = 0;
    } else {
      buf[j] = path[i];
      j++;
    }
  } while (path[i++] != '\0');

  return dir;
}

static directory_entry_t * open_file_from_path(const char *path) {
  char * dir = malloc(strlen(path));
  char filename[256];
  split_dir_filename(path, dir, filename);

  directory_t * directory = open_dir_from_path(dir);
  free(dir);

  directory_entry_t *dir_entry = directory->entries;
  while (dir_entry) {
    if (strcmp(dir_entry->name, filename) == 0) {
      free(directory);
      // TODO: free de la liste chainée.
      return dir_entry;
    }
    dir_entry = dir_entry->next;
  }

  free(directory);
  // TODO: free de la liste chainée.
  
  return NULL;
}

static void init_dir_cluster(int cluster) {
  int n_dir_entries = fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster / sizeof(fat_dir_entry_t);
  fat_dir_entry_t * dir_entries = calloc(n_dir_entries, sizeof(fat_dir_entry_t));
 
  write_data(dir_entries, sizeof(fat_dir_entry_t) * n_dir_entries, fat_info.addr_data + (cluster - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
	free(dir_entries);
}

static int add_fat_dir_entry(char * path, fat_dir_entry_t *fentry, int n) {
  directory_t *dir = open_dir_from_path(path);
  int next = dir->cluster;
  if (next > 0) {
    int n_clusters = 0;
    while (!is_last_cluster(next)) {
      next = fat_info.file_alloc_table[next];
      n_clusters++;
    }
  
    int n_dir_entries = fat_info.BS.bytes_per_sector * fat_info.BS.sectors_per_cluster / sizeof(fat_dir_entry_t);
    fat_dir_entry_t * dir_entries = malloc(n_dir_entries * sizeof(fat_dir_entry_t) * n_clusters);
  
    int c = 0;
    next = dir->cluster;
    while (!is_last_cluster(next)) {
      read_data(dir_entries + c * n_dir_entries, n_dir_entries * sizeof(fat_dir_entry_t), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
      next = fat_info.file_alloc_table[next];
      c++;
    }
  
    int i;
    int consecutif = 0;
    for (i = 0; i < n_dir_entries * n_clusters; i++) {
      if (dir_entries[i].utf8_short_name[0] == 0 || dir_entries[i].utf8_short_name[0] == 0xe5) {
        consecutif++;
        if (consecutif == n) {
          next = dir->cluster;
          int j;
          for (j = 0; j < (i - n + 1) / n_dir_entries; j++)
            next = fat_info.file_alloc_table[next];
          for (j = 0; j < n; j++) {
            int off = (i - n + j + 1) % n_dir_entries; // offset dans le cluster.
            write_data(&fentry[j], sizeof(fat_dir_entry_t), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector + off * sizeof(fat_dir_entry_t));
            if (off == n_dir_entries - 1) {
              next = fat_info.file_alloc_table[next];
            }
          }
          return 0;
        }
      } else {
        consecutif = 0;
      }
    }
    if (consecutif < n) {
      int j;
      int newcluster = alloc_cluster(1);
      init_dir_cluster(newcluster);
      next = dir->cluster;
      while (!is_last_cluster(fat_info.file_alloc_table[next])) {
        next = fat_info.file_alloc_table[next];
      }
      fat_info.file_alloc_table[next] = newcluster;
      fprintf(debug, "new cluster : %d %x\n", newcluster, fat_info.addr_data + (newcluster - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
      fflush(debug);
  
      for (j = 0; j < consecutif; j++) {
        int off = n_dir_entries - consecutif + j;
        write_data(&fentry[j], sizeof(fat_dir_entry_t), fat_info.addr_data + (next - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector + off * sizeof(fat_dir_entry_t));
      }
      for (j = consecutif; j < n; j++) {
        int off = n_dir_entries - consecutif + j;
        write_data(&fentry[j], sizeof(fat_dir_entry_t), fat_info.addr_data + (newcluster - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector + off * sizeof(fat_dir_entry_t));
      }
      return 0;
    }
  } else if (fat_info.fat_type != FAT32) {
    int i;
    int consecutif = 0;
    fat_dir_entry_t *root_dir = malloc(sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count);
    read_data(root_dir, sizeof(fat_dir_entry_t) * fat_info.BS.root_entry_count, fat_info.addr_root_dir);

    for (i = 0; i < fat_info.BS.root_entry_count; i++) {
      if (root_dir[i].utf8_short_name[0] == 0 || root_dir[i].utf8_short_name[0] == 0xe5) {
        consecutif++;
        if (consecutif == n) {
          write_data(fentry, sizeof(fat_dir_entry_t) * n, fat_info.addr_root_dir + (i - n + 1) * sizeof(fat_dir_entry_t));
          return 0;
        }
      }
    }
  }
  return 1;
}

static int fat_utimens(const char *path, const struct timespec tv[2]) {
  char * dir = malloc(strlen(path));
  char filename[256];
  split_dir_filename(path, dir, filename);

  directory_t * directory = open_dir_from_path(dir);
  free(dir);
  
  int ret = updatedate_dir_entry(directory->cluster, filename, tv[0].tv_sec, tv[1].tv_sec);

  free(directory);

  return ret;
}

static int fat_mkdir (const char * path, mode_t mode) {
  fprintf(debug, "fat_mkdir %s %d\n", path, mode);
  fflush(debug);

  char * dir = malloc(strlen(path));
  char filename[256];
  split_dir_filename(path, dir, filename);

  char * sfn = lfn_to_sfn(filename);
  
  int n_entries = 1 + ((strlen(filename) - 1) / 13);
  lfn_entry_t * long_file_name = malloc(sizeof(lfn_entry_t) * (n_entries + 1));
  fat_dir_entry_t *fentry = (fat_dir_entry_t*) &long_file_name[n_entries];

  encode_long_file_name(filename, long_file_name, n_entries);

  strncpy(fentry->utf8_short_name, sfn, 8);
  strncpy(fentry->file_extension, sfn + 8, 3);
  fentry->file_attributes = 0x10; //TODO: Utiliser variable mode et des defines.
  fentry->reserved = 0;
  fentry->create_time_ms = 0;
  time_t t = time(NULL);
  convert_time_t_to_datetime_fat(t, &(fentry->create_time), &(fentry->create_date));
  convert_time_t_to_datetime_fat(t, NULL, &(fentry->last_access_date));
  fentry->ea_index = 0; //XXX
  convert_time_t_to_datetime_fat(t, &(fentry->last_modif_time), &(fentry->last_modif_date));
  fentry->file_size = 0;
  fentry->cluster_pointer = alloc_cluster(1);
  init_dir_cluster(fentry->cluster_pointer);

  add_fat_dir_entry(dir, (fat_dir_entry_t*)long_file_name, n_entries + 1);

  return 0;
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

    directory_entry_t *dir_entry = dir->entries;
    while (dir_entry) {
      if (strcmp(dir_entry->name, filename) == 0) {
          break;
      }
      dir_entry = dir_entry->next;
    }
    if (!dir_entry) {
      free(dir); // TODO: liberer memoire liste chainee.
      return -ENOENT;
    } else {
      if (dir_entry->attributes & 0x01) { // Read Only
        stbuf->st_mode &= ~0111;
      }
      if (dir_entry->attributes & 0x10) { // Dir.
        stbuf->st_mode |= S_IFDIR;
      } else {
        stbuf->st_mode |= S_IFREG;
      }
      stbuf->st_atime = dir_entry->access_time;
      stbuf->st_mtime = dir_entry->modification_time;
      stbuf->st_ctime = dir_entry->creation_time;
      stbuf->st_size = dir_entry->size;
    }
    free(dir); // TODO: liberer memoire liste chainee.
  }

  return res;
}

static int fat_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
  (void) offset;
  (void) fi;
  directory_t *dir;

  fprintf(debug, "fat_readdir %s\n", path);
  fflush(debug);

  if ((dir = open_dir_from_path(path)) == NULL)
    return -ENOENT;

  directory_entry_t *dir_entry = dir->entries;
  while (dir_entry) {
    filler(buf, dir_entry->name, NULL, 0);
    dir_entry = dir_entry->next;
  }
  free(dir); // TODO: libérer liste chainée.

  return 0;
}

static int fat_open(const char *path, struct fuse_file_info *fi)
{
  directory_entry_t *f;

  if ((f = open_file_from_path(path)) == NULL)
    return -ENOENT;

  free(f);

  return 0;
}

static int fat_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
  directory_entry_t *f;
  int count = 0;
  
  if ((f = open_file_from_path(path)) == NULL)
    return -ENOENT;

  if (offset >= f->size) {
    return 0;
  }

  if (size + offset > f->size) {
    size = f->size - offset;
  }

  int fd = open(options.device, O_RDONLY);

  int cluster = f->cluster;

  // Offset
  if (offset > 0) {
    while (offset > fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector) {
      cluster = fat_info.file_alloc_table[cluster];
      offset -= fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector;
    }
    size_t size2 = fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector - offset;
    if (size2 > size)
      size2 = size;
    pread(fd, buf, size2, fat_info.addr_data + offset + (cluster - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
    size -= size2;
    count += size2;
    cluster = fat_info.file_alloc_table[cluster];
  }

  while (size) {
    size_t size2 = fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector;
    if (size2 > size)
      size2 = size;
    pread(fd, buf + count, size2, fat_info.addr_data + (cluster - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
    size -= size2;
    count += size2;
    cluster = fat_info.file_alloc_table[cluster];
  } 
  
  close(fd);

  free(f);

  return count;
}

static int fat_write (const char *path, const char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi)
{
  directory_entry_t *f;
  int count = 0;
  
  if ((f = open_file_from_path(path)) == NULL)
    return -ENOENT;

  if (offset >= f->size) {
    return 0;
  }

  if (size + offset > f->size) {
    size = f->size - offset;
  }

  int fd = open(options.device, O_WRONLY);

  int cluster = f->cluster;

  // Offset
  if (offset > 0) {
    while (offset > fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector) {
      cluster = fat_info.file_alloc_table[cluster];
      offset -= fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector;
    }
    size_t size2 = fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector - offset;
    if (size2 > size)
      size2 = size;
    pwrite(fd, buf, size2, fat_info.addr_data + offset + (cluster - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
    size -= size2;
    count += size2;
    cluster = fat_info.file_alloc_table[cluster];
  }

  while (size) {
    size_t size2 = fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector;
    if (size2 > size)
      size2 = size;
    pwrite(fd, buf + count, size2, fat_info.addr_data + (cluster - 2) * fat_info.BS.sectors_per_cluster * fat_info.BS.bytes_per_sector);
    size -= size2;
    count += size2;
    cluster = fat_info.file_alloc_table[cluster];
  } 
  
  close(fd);

  free(f);

  return count;
}

static int fat_mknod(const char * path, mode_t mode, dev_t dev) {
	char * dir = malloc(strlen(path));
  char filename[256];
  split_dir_filename(path, dir, filename);

  char * sfn = lfn_to_sfn(filename);
  
  int n_entries = 1 + ((strlen(filename) - 1) / 13);
  lfn_entry_t * long_file_name = malloc(sizeof(lfn_entry_t) * (n_entries + 1));
  fat_dir_entry_t *fentry = (fat_dir_entry_t*) &long_file_name[n_entries];

  encode_long_file_name(filename, long_file_name, n_entries);

  strncpy(fentry->utf8_short_name, sfn, 8);
  strncpy(fentry->file_extension, sfn + 8, 3);
  fentry->file_attributes = 0x0; //TODO: Utiliser variable mode.
  fentry->reserved = 0;
  fentry->create_time_ms = 0;
  time_t t = time(NULL);
  convert_time_t_to_datetime_fat(t, &(fentry->create_time), &(fentry->create_date));
  convert_time_t_to_datetime_fat(t, NULL, &(fentry->last_access_date));
  fentry->ea_index = 0; //XXX
  convert_time_t_to_datetime_fat(t, &(fentry->last_modif_time), &(fentry->last_modif_date));
  fentry->file_size = 0;
  fentry->cluster_pointer = alloc_cluster(1);
  init_dir_cluster(fentry->cluster_pointer);

  add_fat_dir_entry(dir, (fat_dir_entry_t*)long_file_name, n_entries + 1);

	free(dir);
	return 0;
}

static int fat_chmod(const char * path, mode_t mode) {
  return 0;
}

static int fat_chown(const char * path, uid_t uid, gid_t gid) {
  return 0;
}

static int fat_truncate(const char * path, off_t off) {
  return 0;
}

static int fat_unlink(const char * path) {
  if (path[0] == '\0' || strcmp(path, "/") == 0)
    return -1;

  // Only absolute paths.
  if (path[0] != '/')
    return -1;

  char buf[256];
  int i = 1;
  while (path[i] == '/')
    i++;

  directory_t * dir = open_root_dir();

  int j = 0;
  do {
    if (path[i] == '/' || path[i] == '\0') {
      buf[j] = '\0';

			int cluster = dir->cluster;
      if (j > 0 && open_next_dir(dir, dir, buf) == 2) {
				  fprintf(debug, "delete, name = %s\n", buf);
					delete_file_dir(cluster, buf);
				  fflush(debug);


					return 0;
			}

      j = 0;
    } else {
      buf[j] = path[i];
      j++;
    }
  } while (path[i++] != '\0');

	return 1;
}

static struct fuse_operations fat_oper = {
    .chmod = fat_chmod,
    .chown = fat_chown,
		.mknod = fat_mknod,
    .getattr  = fat_getattr,
    .mkdir = fat_mkdir,
    .open = fat_open,
    .read = fat_read,
    .readdir  = fat_readdir,
    .truncate = fat_truncate,
    .utimens = fat_utimens,
    .write = fat_write,
		.unlink = fat_unlink,
};

int main(int argc, char *argv[])
{
  int ret;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct fat_state *fat_data;
        
  if (fuse_opt_parse(&args, &options, fat_fuse_opts, NULL) == -1)
    return -1; /** error parsing **/

  fprintf(stderr, "device : %s\n", options.device);

  debug = fopen("/tmp/debugfuse", "w+");
  mount_fat();
  
  ret = fuse_main(args.argc, args.argv, &fat_oper, fat_data);
  fuse_opt_free_args(&args);

  return ret;
}
