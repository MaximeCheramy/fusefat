#ifndef __FAT_H__
#define __FAT_H__

#include <stdint.h>

typedef struct _fat_BS {
// Boot Sector
  uint8_t   bootjmp[3];         //0x00
  uint8_t   oem_name[8];        //0x03
  uint16_t  bytes_per_sector;     //0x0b
  uint8_t   sectors_per_cluster;    //0x0d
  uint16_t  reserved_sector_count;    //0x0e
  uint8_t   table_count;        //0x10
  uint16_t  root_entry_count;     //0x11
  uint16_t  total_sectors_16;     //0x13
  uint8_t   media_type;         //0x15
  uint16_t  table_size_16;        //0x16
  uint16_t  sectors_per_track;      //0x18
  uint16_t  head_side_count;      //0x1a
  uint32_t  hidden_sector_count;    //0x1c
  uint32_t  total_sectors_32;     //0x20

// Extended BIOS Parameter Block (FAT12 / FAT16)
  uint8_t   bios_drive_num;       //0x24
  uint8_t   reserved;         //0x25
  uint8_t   boot_signature;       //0x26
  uint32_t  volume_id;          //0x27
  uint8_t   volume_label[11];     //0x2b
  uint8_t   fat_type_label[8];      //0x36
  uint8_t   os_boot_code[448];      //0x3e
} __attribute__ ((packed)) fat_BS_t;  

typedef struct _fat_dir_entry {
  uint8_t   utf8_short_name[8];
  uint8_t   file_extension[3];
  uint8_t   file_attributes;
  uint8_t   reserved;
  uint8_t   create_time_ms;
  uint16_t  create_time;
  uint16_t  create_date;
  uint16_t  last_access_date;
  uint16_t  ea_index;
  uint16_t  last_modif_time;
  uint16_t  last_modif_date;
  uint16_t  cluster_pointer;
  uint32_t  file_size;

}__attribute__((packed)) fat_dir_entry_t;

typedef struct _directory_entry {
  char name[256];
  uint8_t attributes;
  uint32_t size;
  time_t access_time;
  time_t modification_time;
  time_t creation_time;
  uint32_t cluster;
} directory_entry_t;

typedef struct _directory {
  directory_entry_t entries[10];
  int total_entries;
  char name[256];
} directory_t;

typedef struct {
  uint8_t   seq_number;
  uint8_t   filename1[10];
  uint8_t   attributes;
  uint8_t   reserved; // always 0x0
  uint8_t   checksum;
  uint8_t   filename2[12];
  uint16_t  cluster_pointer; // always 0x000
  uint8_t   filename3[4];
}__attribute__((packed)) lfn_entry_t;


typedef struct _fat_info {
  fat_BS_t BS;
  unsigned int *addr_fat;
  unsigned int addr_root_dir;
  unsigned int addr_data;
  unsigned int *file_alloc_table;
  unsigned int total_clusters;
} fat_info_t;

typedef struct _path {
  int n_dirs;
  char** names;
} path_t;

#endif
